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
        GUILDHOOK_CAN_ADD_MEMBER
    }) {}

    bool CanGuildAddMember(Guild* guild, Player* player, uint8& /*plRank*/) override
    {
        if (!player || !GET_PLAYERBOT_AI(player))
            return true;  // not a bot — always allow

        // Arena team bots must never join real guilds.
        if (sPlayerbotAIConfig.IsArenaTeamBot(player->GetGUID()))
            return false;

        if (!sGuildBotMgr.enabled || !sGuildBotMgr.maxBotsInGuild)
            return true;

        if (!sRandomPlayerbotMgr.IsRndBotAccount(player->GetSession()->GetAccountId()))
            return true;

        uint32 guildId = guild->GetId();
        if (!sGuildBotMgr.IsRealGuild(guildId))
            return true;

        uint32 botCount = sGuildBotMgr.GetBotCountInGuild(guildId);
        if (botCount < sGuildBotMgr.maxBotsInGuild)
            return true;

        LOG_DEBUG("playerbots", "mod-guild-bots: blocking bot {} from joining guild {} (limit {}/{}).",
            player->GetName(), guild->GetName(), botCount, sGuildBotMgr.maxBotsInGuild);
        return false;
    }
};

void AddGuildBotScripts()
{
    new GuildBotWorldScript();
    new GuildBotPlayerScript();
    new GuildBotGuildScript();
}
