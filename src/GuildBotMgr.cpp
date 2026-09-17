/*
 * This file is part of the mod-guild-bots module for AzerothCore.
 * Released under GNU GPL v2 license.
 */

#include "GuildBotMgr.h"

#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <ctime>

void GuildBotMgr::Initialize(bool /*reload*/)
{
    enabled       = sConfigMgr->GetOption<bool>("GuildBot.Enable", true);
    minOnline     = sConfigMgr->GetOption<uint32>("GuildBot.MinOnline", 40);
    maxBotsInGuild = sConfigMgr->GetOption<uint32>("GuildBot.MaxBotsInGuild", 40);

    if (!enabled)
        return;

    LOG_INFO("server.loading", "mod-guild-bots: initialized (Enable={}, MinOnline={}, MaxBotsInGuild={}).",
        enabled, minOnline, maxBotsInGuild);
}

// ---------------------------------------------------------------------------
// Main update — called every world tick.
// ---------------------------------------------------------------------------

void GuildBotMgr::Update(uint32 diff)
{
    if (!enabled || !minOnline)
        return;

    // One staggered logout per tick.
    ProcessStaggeredLogout();

    _checkTimer += diff;
    if (_checkTimer < CHECK_INTERVAL_MS)
        return;
    _checkTimer = 0;

    PeriodicCheck();
    CheckInstanceEvictions();
}

// ---------------------------------------------------------------------------
// Periodic 30-second guild health check.
// ---------------------------------------------------------------------------

void GuildBotMgr::PeriodicCheck()
{
    std::vector<Player*> const realPlayers = sRandomPlayerbotMgr.GetPlayers();
    if (realPlayers.empty())
        return;

    // Precompute online managed-bot count per guild in one pass.
    std::unordered_map<uint32, uint32> onlinePerGuild;
    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (bot && bot->IsInWorld() && bot->GetGuildId())
            ++onlinePerGuild[bot->GetGuildId()];
    }

    time_t now = time(nullptr);
    std::unordered_set<uint32> checkedGuilds;

    for (Player* player : realPlayers)
    {
        if (!player || !player->IsInWorld())
            continue;

        uint32 guildId = player->GetGuildId();
        if (!guildId || !checkedGuilds.insert(guildId).second)
            continue;

        if (!IsRealGuild(guildId))
            continue;

        uint32 online = onlinePerGuild.count(guildId) ? onlinePerGuild[guildId] : 0;
        if (online >= minOnline)
            continue;

        // Rate-limit DB queries per guild.
        time_t& last = _lastCheck[guildId];
        if (now - last < RATE_LIMIT_SECS)
            continue;
        last = now;

        EnsureGuildBotsOnline(guildId, online);
    }
}

// ---------------------------------------------------------------------------
// Instance eviction: if a managed bot is in a dungeon/raid with no real
// player on the same map, leave the group and teleport home.
// ---------------------------------------------------------------------------

void GuildBotMgr::CheckInstanceEvictions()
{
    std::vector<ObjectGuid> toEvict;

    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
            continue;

        Map* map = bot->GetMap();
        if (!map || (!map->IsDungeon() && !map->IsRaid()))
            continue;

        bool realPlayerPresent = false;
        Group* group = bot->GetGroup();
        if (group)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (member && !GET_PLAYERBOT_AI(member) && member->GetMap() == map)
                {
                    realPlayerPresent = true;
                    break;
                }
            }
        }

        if (!realPlayerPresent)
            toEvict.push_back(guid);
    }

    for (ObjectGuid const& guid : toEvict)
    {
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
            continue;

        LOG_DEBUG("playerbots", "mod-guild-bots: evicting bot {} from instance {} (no real player remaining).",
            bot->GetName(), bot->GetMap() ? bot->GetMap()->GetMapName() : "?");

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (bot->GetGroup() && botAI)
            botAI->LeaveOrDisbandGroup();

        bot->TeleportTo(bot->m_homebindMapId, bot->m_homebindX, bot->m_homebindY, bot->m_homebindZ,
            bot->GetOrientation());
    }
}

// ---------------------------------------------------------------------------
// Real-player login / logout triggers.
// ---------------------------------------------------------------------------

void GuildBotMgr::OnRealPlayerLogin(Player* player)
{
    if (!enabled || !minOnline)
        return;

    uint32 guildId = player->GetGuildId();
    if (!guildId)
        return;

    if (!IsRealGuild(guildId))
        return;

    // Cancel any pending logouts for bots in this guild.
    _pendingLogouts.erase(
        std::remove_if(_pendingLogouts.begin(), _pendingLogouts.end(),
            [&](ObjectGuid const& guid)
            {
                Player* bot = ObjectAccessor::FindPlayer(guid);
                return bot && bot->GetGuildId() == guildId;
            }),
        _pendingLogouts.end());

    // Bypass rate limit so bots log in immediately on player arrival.
    _lastCheck[guildId] = 0;

    uint32 online = GetOnlineCount(guildId);
    if (online < minOnline)
        EnsureGuildBotsOnline(guildId, online);
}

void GuildBotMgr::OnRealPlayerLogout(Player* player)
{
    if (!enabled || !minOnline)
        return;

    uint32 guildId = player->GetGuildId();
    if (!guildId)
        return;

    if (!IsRealGuild(guildId))
        return;

    // Check if this was the last real player in the guild.
    // The logging-out player is still technically online here, so skip them.
    bool otherRealPlayerOnline = false;
    for (Player* p : sRandomPlayerbotMgr.GetPlayers())
    {
        if (p == player || !p->IsInWorld())
            continue;
        if (p->GetGuildId() == guildId)
        {
            otherRealPlayerOnline = true;
            break;
        }
    }

    if (!otherRealPlayerOnline)
        EnsureGuildBotsOffline(guildId);
}

// ---------------------------------------------------------------------------
// Core login logic.
// ---------------------------------------------------------------------------

void GuildBotMgr::EnsureGuildBotsOnline(uint32 guildId, uint32 currentOnline)
{
    if (currentOnline == UINT32_MAX)
        currentOnline = GetOnlineCount(guildId);

    if (currentOnline >= minOnline)
        return;

    uint32 toLogin = minOnline - currentOnline;

    QueryResult result = CharacterDatabase.Query(
        "SELECT gm.guid, c.account FROM guild_member gm "
        "INNER JOIN characters c ON c.guid = gm.guid "
        "WHERE gm.guildid = {}", guildId);

    if (!result)
        return;

    do
    {
        if (!toLogin)
            break;

        uint32 charGuid  = (*result)[0].Get<uint32>();
        uint32 accountId = (*result)[1].Get<uint32>();

        if (!sRandomPlayerbotMgr.IsRndBotAccount(accountId))
            continue;

        ObjectGuid botGUID = ObjectGuid::Create<HighGuid::Player>(charGuid);

        if (sPlayerbotAIConfig.IsArenaTeamBot(botGUID))
            continue;

        if (sRandomPlayerbotMgr.GetPlayerBot(botGUID))
            continue;  // already online

        _managedBots.insert(charGuid);
        // NOTE: AddPlayerBot without inserting into currentBots keeps guild bots
        // outside the MaxRandomBots cap tracked by RandomPlayerbotMgr.
        sRandomPlayerbotMgr.AddPlayerBot(botGUID, 0);

        LOG_DEBUG("playerbots", "mod-guild-bots: logging in bot {} for guild {}.", charGuid, guildId);
        --toLogin;

    } while (result->NextRow());
}

// ---------------------------------------------------------------------------
// Core logout logic.
// ---------------------------------------------------------------------------

void GuildBotMgr::EnsureGuildBotsOffline(uint32 guildId)
{
    if (HasRealPlayerInGuild(guildId))
        return;

    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
            continue;
        if (bot->GetGuildId() != guildId)
            continue;

        // Arena team bots have their own always-online logic — don't touch them.
        if (sPlayerbotAIConfig.IsArenaTeamBot(guid))
            continue;

        // Skip if already in the queue.
        if (std::find(_pendingLogouts.begin(), _pendingLogouts.end(), guid) != _pendingLogouts.end())
            continue;

        _pendingLogouts.push_back(guid);
    }
}

// ---------------------------------------------------------------------------
// Stagger: one logout per world tick.
// ---------------------------------------------------------------------------

void GuildBotMgr::ProcessStaggeredLogout()
{
    if (_pendingLogouts.empty())
        return;

    ObjectGuid guid = _pendingLogouts.front();
    _pendingLogouts.pop_front();

    Player* bot = ObjectAccessor::FindPlayer(guid);
    if (!bot || !bot->IsInWorld())
    {
        _managedBots.erase(guid.GetCounter());
        return;
    }

    // A real player came back to the guild — keep the bot online.
    if (HasRealPlayerInGuild(bot->GetGuildId()))
        return;

    uint32 guidLow = guid.GetCounter();
    _managedBots.erase(guidLow);

    LOG_DEBUG("playerbots", "mod-guild-bots: stagger-logout for bot {}.", bot->GetName());
    sRandomPlayerbotMgr.LogoutPlayerBot(guid);
}

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

uint32 GuildBotMgr::GetOnlineCount(uint32 guildId) const
{
    uint32 count = 0;
    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (bot && bot->IsInWorld() && bot->GetGuildId() == guildId)
            ++count;
    }
    return count;
}

bool GuildBotMgr::HasRealPlayerInGuild(uint32 guildId) const
{
    for (Player* player : sRandomPlayerbotMgr.GetPlayers())
        if (player && player->IsInWorld() && player->GetGuildId() == guildId)
            return true;
    return false;
}

uint32 GuildBotMgr::GetBotCountInGuild(uint32 guildId) const
{
    QueryResult result = CharacterDatabase.Query(
        "SELECT c.account FROM guild_member gm "
        "INNER JOIN characters c ON c.guid = gm.guid "
        "WHERE gm.guildid = {}", guildId);

    if (!result)
        return 0;

    uint32 count = 0;
    do
    {
        uint32 accountId = (*result)[0].Get<uint32>();
        if (sRandomPlayerbotMgr.IsRndBotAccount(accountId))
            ++count;
    } while (result->NextRow());

    return count;
}

bool GuildBotMgr::IsRealGuild(uint32 guildId) const
{
    if (!guildId)
        return false;

    QueryResult result = CharacterDatabase.Query(
        "SELECT c.account FROM guild_member gm "
        "JOIN characters c ON gm.guid = c.guid "
        "WHERE gm.guildid = {}", guildId);

    if (!result)
        return false;

    do
    {
        uint32 accountId = (*result)[0].Get<uint32>();
        if (!sRandomPlayerbotMgr.IsRndBotAccount(accountId))
            return true;
    } while (result->NextRow());

    return false;
}
