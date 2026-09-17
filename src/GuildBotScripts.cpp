/*
 * This file is part of the mod-guild-bots module for AzerothCore.
 * Released under GNU GPL v2 license.
 */

#include "GuildBotMgr.h"
#include "Guild.h"
#include "Player.h"
#include "Playerbots.h"
#include "PlayerbotAIConfig.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"

class GuildBotWorldScript : public WorldScript
{
public:
    GuildBotWorldScript() : WorldScript("GuildBotWorldScript",
    {
        WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
        WORLDHOOK_ON_UPDATE
    }) {}

    void OnBeforeConfigLoad(bool reload) override
    {
        sGuildBotMgr.Initialize(reload);
    }

    void OnUpdate(uint32 diff) override
    {
        sGuildBotMgr.Update(diff);
    }
};

class GuildBotPlayerScript : public PlayerScript
{
public:
    GuildBotPlayerScript() : PlayerScript("GuildBotPlayerScript",
    {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (!player || GET_PLAYERBOT_AI(player))
            return;
        sGuildBotMgr.OnRealPlayerLogin(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player || GET_PLAYERBOT_AI(player))
            return;
        sGuildBotMgr.OnRealPlayerLogout(player);
    }
};

class GuildBotGuildScript : public GuildScript
{
public:
    GuildBotGuildScript() : GuildScript("GuildBotGuildScript",
    {
        GUILDHOOK_CAN_ADD_MEMBER,
        GUILDHOOK_ON_REMOVE_MEMBER
    }) {}

    bool CanGuildAddMember(Guild* guild, Player* player, uint8& /*plRank*/) override
    {
        if (!player || !GET_PLAYERBOT_AI(player))
            return true;  // not a bot — always allow

        // Arena team bots must never join real guilds.
        if (sPlayerbotAIConfig.IsArenaTeamBot(player->GetGUID()))
            return false;

        uint32 accountId = player->GetSession()->GetAccountId();
        if (!sRandomPlayerbotMgr.IsRndBotAccount(accountId))
            return true;

        uint32 guildId = guild->GetId();
        if (!sGuildBotMgr.IsRealGuild(guildId))
            return true;

        // Mark this bot account as guild-bot type so it's excluded from random pool.
        sGuildBotMgr.MarkAsGuildBotAccount(accountId);

        if (!sGuildBotMgr.enabled || !sGuildBotMgr.maxBotsInGuild)
            return true;

        uint32 botCount = sGuildBotMgr.GetBotCountInGuild(guildId);
        if (botCount < sGuildBotMgr.maxBotsInGuild)
            return true;

        LOG_DEBUG("playerbots", "mod-guild-bots: blocking bot {} from joining guild {} (limit {}/{}).",
            player->GetName(), guild->GetName(), botCount, sGuildBotMgr.maxBotsInGuild);
        return false;
    }

    void OnRemoveMember(Guild* /*guild*/, Player* player, bool /*isDisbanding*/, bool /*isKicked*/) override
    {
        if (!player || !GET_PLAYERBOT_AI(player))
            return;

        uint32 accountId = player->GetSession()->GetAccountId();
        if (!sRandomPlayerbotMgr.IsRndBotAccount(accountId))
            return;

        // Restore to random pool if bot is no longer in any real guild.
        sGuildBotMgr.UnmarkAsGuildBotAccount(accountId);
    }
};

void AddGuildBotScripts()
{
    new GuildBotWorldScript();
    new GuildBotPlayerScript();
    new GuildBotGuildScript();
}
