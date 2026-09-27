/*
 * This file is part of the mod-guild-bots module for AzerothCore.
 * Released under GNU GPL v2 license.
 */

#include "GuildBotMgr.h"

#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"

#include <algorithm>
#include <ctime>
#include <vector>

void GuildBotMgr::Initialize(bool /*reload*/)
{
    enabled        = sConfigMgr->GetOption<bool>("GuildBot.Enable", true);
    minOnline      = sConfigMgr->GetOption<uint32>("GuildBot.MinOnline", 40);
    maxBotsInGuild  = sConfigMgr->GetOption<uint32>("GuildBot.MaxBotsInGuild", 40);
    botsPerInterval = sConfigMgr->GetOption<uint32>("GuildBot.BotsPerInterval", 50);

    // Always mark guild-bot accounts even when the module is disabled so they
    // are never picked up by the random bot pool.
    MarkExistingGuildBotAccounts();

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

    // One staggered login and one logout per tick.
    ProcessStaggeredLogin();
    ProcessStaggeredLogout();

    _masterSyncTimer += diff;
    if (_masterSyncTimer >= MASTER_SYNC_INTERVAL_MS)
    {
        _masterSyncTimer = 0;
        MasterSync();
    }

    _checkTimer += diff;
    if (_checkTimer < CHECK_INTERVAL_MS)
        return;
    _checkTimer = 0;

    PeriodicCheck();
}

// ---------------------------------------------------------------------------
// Master sync: set or clear the PlayerbotAI master pointer for each managed
// bot based on current group membership.  Uses GetMemberSlots (not
// GroupReference) so a real player in a portal map-transition is not mistaken
// for "no real player" and does not trigger a premature group leave.
//
// AI driving (ProcessBot / randomize / teleport) is now handled by
// RandomPlayerbotMgr::UpdateAIInternal via the guildBots set; MasterSync only
// manages the master pointer so packet dispatch (CMSG_AREATRIGGER, etc.) works.
// ---------------------------------------------------------------------------

void GuildBotMgr::MasterSync()
{
    if (_managedBots.empty())
        return;

    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (!bot || !bot->IsInWorld())
            continue;

        PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
        if (!botAI)
            continue;

        Group* group = bot->GetGroup();

        if (!group || group->isLFGGroup())
        {
            if (botAI->GetMaster())
            {
                botAI->SetMaster(nullptr);
                botAI->ResetStrategies();
                sRandomPlayerbotMgr.ResetIdleTimers(guidLow);
            }
            continue;
        }

        // Scan group members using GetMemberSlots to cover offline/transitioning players.
        bool hasRealPlayer = false;
        Player* newMaster = nullptr;

        for (Group::MemberSlotList::const_iterator i = group->GetMemberSlots().begin();
             i != group->GetMemberSlots().end(); ++i)
        {
            Player* member = ObjectAccessor::FindPlayer(i->guid);
            if (member)
            {
                if (!GET_PLAYERBOT_AI(member) || IsSelfBot(member))
                {
                    hasRealPlayer = true;
                    newMaster = member;
                    break;
                }
            }
            else
            {
                // Member is offline or mid-map-transition.  Check account type:
                // real players are never in the random-bot account pool.
                uint32 acctId = sCharacterCache->GetCharacterAccountIdByGuid(i->guid);
                if (acctId && !sRandomPlayerbotMgr.IsRndBotAccount(acctId))
                {
                    // Keep the existing master so any in-flight portal packet
                    // dispatch is not interrupted.
                    hasRealPlayer = true;
                    newMaster = botAI->GetMaster();
                    break;
                }
            }
        }

        if (hasRealPlayer)
        {
            if (newMaster && botAI->GetMaster() != newMaster)
                botAI->SetMaster(newMaster);
        }
        else
        {
            botAI->LeaveOrDisbandGroup();
            botAI->SetMaster(nullptr);
            botAI->ResetStrategies();
            sRandomPlayerbotMgr.ResetIdleTimers(guidLow);
        }
    }
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

    // Sync master immediately so portal/follow dispatch is ready without
    // waiting for the next 15-second MasterSync tick.
    MasterSync();
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

    // Pre-fetch all bot account IDs (type 1 = random, type 3 = guild-bot) so we
    // don't rely on the in-memory rndBotTypeAccounts list which only holds type=1.
    QueryResult botTypeResult = PlayerbotsDatabase.Query(
        "SELECT account_id FROM playerbots_account_type WHERE account_type IN (1, 3)");
    std::unordered_set<uint32> botAccountIds;
    if (botTypeResult)
    {
        do
        {
            botAccountIds.insert((*botTypeResult)[0].Get<uint32>());
        } while (botTypeResult->NextRow());
    }

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

        if (!botAccountIds.count(accountId))
            continue;

        ObjectGuid botGUID = ObjectGuid::Create<HighGuid::Player>(charGuid);

        if (sPlayerbotAIConfig.IsArenaTeamBot(botGUID))
            continue;

        if (sRandomPlayerbotMgr.GetPlayerBot(botGUID))
            continue;  // already online

        _managedBots.insert(charGuid);

        // Skip if already queued for login.
        if (std::find(_pendingLogins.begin(), _pendingLogins.end(), botGUID) != _pendingLogins.end())
            continue;

        _pendingLogins.push_back(botGUID);
        LOG_DEBUG("playerbots", "mod-guild-bots: queued bot {} for login (guild {}).", charGuid, guildId);
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
// Stagger: one login per world tick.
// ---------------------------------------------------------------------------

void GuildBotMgr::ProcessStaggeredLogin()
{
    if (_pendingLogins.empty())
        return;

    ObjectGuid guid = _pendingLogins.front();
    _pendingLogins.pop_front();

    // If bot was already logged in by something else, skip.
    if (sRandomPlayerbotMgr.GetPlayerBot(guid))
        return;

    LOG_DEBUG("playerbots", "mod-guild-bots: logging in bot {}.", guid.GetCounter());
    sRandomPlayerbotMgr.AddPlayerBot(guid, 0);
    sRandomPlayerbotMgr.AddGuildBotToAI(guid.GetCounter());
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

    uint32 guidLow = guid.GetCounter();
    Player* bot = ObjectAccessor::FindPlayer(guid);
    if (!bot || !bot->IsInWorld())
    {
        _managedBots.erase(guidLow);
        sRandomPlayerbotMgr.RemoveGuildBotFromAI(guidLow);
        return;
    }

    // A real player came back to the guild — keep the bot online.
    if (HasRealPlayerInGuild(bot->GetGuildId()))
        return;

    _managedBots.erase(guidLow);
    sRandomPlayerbotMgr.RemoveGuildBotFromAI(guidLow);

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
    // Iterate all online players including selfbots (who have PlayerbotAI but
    // are not bot accounts).
    for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
    {
        if (player && player->IsInWorld() && player->GetGuildId() == guildId
            && !sRandomPlayerbotMgr.IsRndBotAccount(player->GetSession()->GetAccountId()))
            return true;
    }
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

void GuildBotMgr::MarkExistingGuildBotAccounts()
{
    // Step 1: get all type=1 bot account IDs from playerbots DB
    QueryResult botAccounts = PlayerbotsDatabase.Query(
        "SELECT account_id FROM playerbots_account_type WHERE account_type = 1");
    if (!botAccounts)
        return;

    std::string botIds;
    do
    {
        if (!botIds.empty())
            botIds += ',';
        botIds += std::to_string((*botAccounts)[0].Get<uint32>());
    } while (botAccounts->NextRow());

    if (botIds.empty())
        return;

    // Step 2: from CharacterDB find which of those are in real guilds
    // (guild has at least one member whose account is NOT in botIds)
    QueryResult guildBots = CharacterDatabase.Query(
        "SELECT DISTINCT c.account FROM guild_member gm "
        "INNER JOIN characters c ON c.guid = gm.guid "
        "WHERE c.account IN ({}) "
        "AND EXISTS ("
        "  SELECT 1 FROM guild_member gm2 "
        "  INNER JOIN characters c2 ON c2.guid = gm2.guid "
        "  WHERE gm2.guildid = gm.guildid "
        "  AND c2.account NOT IN ({})"
        ")", botIds, botIds);

    if (!guildBots)
        return;

    std::string toMark;
    do
    {
        if (!toMark.empty())
            toMark += ',';
        toMark += std::to_string((*guildBots)[0].Get<uint32>());
    } while (guildBots->NextRow());

    if (toMark.empty())
        return;

    // Step 3: mark them as type 3 in playerbots DB
    PlayerbotsDatabase.Execute(
        "UPDATE playerbots_account_type SET account_type = 3 "
        "WHERE account_id IN ({}) AND account_type = 1", toMark);

    LOG_INFO("playerbots", "mod-guild-bots: marked {} account(s) as guild-bot type.", toMark.size());
}

void GuildBotMgr::MarkAsGuildBotAccount(uint32 accountId)
{
    PlayerbotsDatabase.Execute(
        "UPDATE playerbots_account_type SET account_type = 3 "
        "WHERE account_id = {} AND account_type = 1", accountId);
}

bool GuildBotMgr::UnmarkAsGuildBotAccount(uint32 accountId)
{
    // Only restore if the bot has no remaining real guilds.
    QueryResult result = CharacterDatabase.Query(
        "SELECT 1 FROM characters c "
        "INNER JOIN guild_member gm ON gm.guid = c.guid "
        "WHERE c.account = {} "
        "AND EXISTS ("
        "  SELECT 1 FROM guild_member gm2 "
        "  INNER JOIN characters c2 ON c2.guid = gm2.guid "
        "  WHERE gm2.guildid = gm.guildid "
        "  AND c2.account NOT IN (SELECT account_id FROM playerbots_account_type)"
        ") LIMIT 1", accountId);

    if (!result)
    {
        PlayerbotsDatabase.Execute(
            "UPDATE playerbots_account_type SET account_type = 1 "
            "WHERE account_id = {} AND account_type = 3", accountId);
        return true;
    }
    return false;
}

void GuildBotMgr::EvictBotsForAccount(uint32 accountId)
{
    std::vector<uint32> toEvict;
    for (uint32 guidLow : _managedBots)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        Player* bot = ObjectAccessor::FindPlayer(guid);
        if (bot && bot->GetSession()->GetAccountId() == accountId)
            toEvict.push_back(guidLow);
    }
    for (uint32 guidLow : toEvict)
    {
        ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(guidLow);
        _managedBots.erase(guidLow);
        sRandomPlayerbotMgr.RemoveGuildBotFromAI(guidLow);
        LOG_DEBUG("playerbots", "mod-guild-bots: bot guid={} evicted from managed set (account removed from guild).", guidLow);
        sRandomPlayerbotMgr.LogoutPlayerBot(guid);
    }
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
