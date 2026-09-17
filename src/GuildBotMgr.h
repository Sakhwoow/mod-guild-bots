/*
 * This file is part of the mod-guild-bots module for AzerothCore.
 * Released under GNU GPL v2 license.
 */

#ifndef MOD_GUILD_BOTS_GUILDBOT_MGR_H
#define MOD_GUILD_BOTS_GUILDBOT_MGR_H

#include "Define.h"
#include "ObjectGuid.h"
#include <ctime>
#include <deque>
#include <unordered_map>
#include <unordered_set>

class Player;

class GuildBotMgr
{
public:
    static GuildBotMgr& instance()
    {
        static GuildBotMgr inst;
        return inst;
    }

    void Initialize(bool reload);

    // Called every world update tick from WorldScript::OnUpdate.
    void Update(uint32 diff);

    // Called from PlayerScript when a real (non-bot) player logs in or out.
    void OnRealPlayerLogin(Player* player);
    void OnRealPlayerLogout(Player* player);

    // True if this char GUID is managed by this module.
    bool IsManagedBot(uint32 charGuidLow) const { return _managedBots.count(charGuidLow) > 0; }

    // Mark a bot account as guild-bot type (3) so it's excluded from the random bot pool.
    void MarkAsGuildBotAccount(uint32 accountId);
    // Restore a bot account to random type (1) when it leaves all real guilds.
    void UnmarkAsGuildBotAccount(uint32 accountId);
    // On startup: retroactively mark all existing guild-bot accounts as type 3.
    void MarkExistingGuildBotAccounts();

    // Count of rndbot accounts currently in the guild (online + offline).
    uint32 GetBotCountInGuild(uint32 guildId) const;

    // True if the guild has any non-bot account member (member-based DB check).
    bool IsRealGuild(uint32 guildId) const;

    // Config values — readable from other scripts if needed.
    uint32 minOnline     = 0;
    uint32 maxBotsInGuild = 0;
    bool   enabled       = false;

private:
    GuildBotMgr() = default;
    GuildBotMgr(GuildBotMgr const&)            = delete;
    GuildBotMgr& operator=(GuildBotMgr const&) = delete;

    // Login up to minOnline bots for the guild.
    // currentOnline: pass the already-computed count to skip a second scan.
    void EnsureGuildBotsOnline(uint32 guildId, uint32 currentOnline = UINT32_MAX);

    // Queue all online guild bots for staggered logout.
    void EnsureGuildBotsOffline(uint32 guildId);

    // Log out one bot from the pending-logout deque.
    void ProcessStaggeredLogout();

    // Periodic 30-second check: ensure guilds with real players have enough bots.
    void PeriodicCheck();

    // Evict managed bots from instances that no longer contain a real player.
    void CheckInstanceEvictions();

    // Count online managed bots for a specific guild.
    uint32 GetOnlineCount(uint32 guildId) const;

    // True if at least one real (non-bot) player is in the guild right now.
    bool HasRealPlayerInGuild(uint32 guildId) const;

    // Char GUID lows for all bots we have logged in via this module.
    std::unordered_set<uint32> _managedBots;

    // Stagger logout queue: one bot per tick.
    std::deque<ObjectGuid> _pendingLogouts;

    // Wall-clock timestamp of the last EnsureGuildBotsOnline call per guild.
    std::unordered_map<uint32, time_t> _lastCheck;

    // Accumulator for WorldScript::OnUpdate diff.
    uint32 _checkTimer = 0;

    static constexpr uint32 CHECK_INTERVAL_MS = 30000; // 30 s between periodic checks
    static constexpr time_t RATE_LIMIT_SECS   = 300;   // 5 min between DB queries per guild
};

#define sGuildBotMgr GuildBotMgr::instance()

#endif // MOD_GUILD_BOTS_GUILDBOT_MGR_H
