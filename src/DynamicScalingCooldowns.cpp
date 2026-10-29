/*
 * mod-dynamic-scaling: cooldown resets in instances, battlegrounds, arenas and duels.
 *
 * Killing a boss (or wiping on it after a while) resets the group's cooldowns, clears Sated / Exhaustion and,
 * optionally, pet cooldowns, the way Wrath Classic did after raid encounters. Each instance kind (dungeon,
 * heroic dungeon, 10 / 25 player raids and their heroic versions) and each kind of creature (bosses, world bosses,
 * elites, rares) can be switched on or off, in the config or, with CooldownReset.UseDBOverride, in the database.
 * Battlegrounds (when the gates open), arenas (on entering) and duels (when one ends, only what was used in it)
 * can reset cooldowns too. Which spells and categories are always or never reset, per place, comes from
 * `dynamic_scaling_cooldown_spell`.
 *
 * The cooldown reset mechanic is by sogladev: the encounter and boss state triggers, the wipe timer, the evade
 * reasons that don't count, what gets reset, and the creature and encounter lists (now rows of
 * `dynamic_scaling_cooldown_reset`).
 */

#include "DynamicScaling.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "Battleground.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Timer.h"

#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace
{
    enum CooldownResetSpells : uint32
    {
        SPELL_EXHAUSTION = 57723,
        SPELL_SATED      = 57724
    };

    // What can be switched on or off (config names, and `dynamic_scaling_cooldown_rule`.`rule`)
    enum CooldownRule : uint8
    {
        RULE_DUNGEON,
        RULE_DUNGEON_HEROIC,
        RULE_RAID_10,
        RULE_RAID_25,
        RULE_RAID_10_HEROIC,
        RULE_RAID_25_HEROIC,
        RULE_BOSS,
        RULE_WORLD_BOSS,
        RULE_ELITE,
        RULE_RARE,
        RULE_BATTLEGROUND,
        RULE_ARENA,
        RULE_DUEL,
        MAX_COOLDOWN_RULE
    };

    struct RuleName
    {
        char const* name;
        bool defaultValue;
    };

    constexpr RuleName RULE_NAMES[MAX_COOLDOWN_RULE] =
    {
        { "Dungeon", false }, { "DungeonHeroic", false }, { "Raid10", true }, { "Raid25", true },
        { "Raid10Heroic", true }, { "Raid25Heroic", true }, { "Boss", true }, { "WorldBoss", true },
        { "Elite", false }, { "Rare", false }, { "Battleground", false }, { "Arena", false }, { "Duel", false }
    };

    // Where a reset happens (`dynamic_scaling_cooldown_spell`.`reset_in` / `never_in` bits)
    enum CooldownContext : uint8
    {
        CONTEXT_INSTANCE     = 0x1,
        CONTEXT_BATTLEGROUND = 0x2,
        CONTEXT_ARENA        = 0x4,
        CONTEXT_DUEL         = 0x8
    };

    // `dynamic_scaling_cooldown_spell`.`type`
    enum CooldownSpellType : uint8
    {
        SPELL_TYPE_SPELL    = 0,
        SPELL_TYPE_CATEGORY = 1
    };

    struct SpellRule
    {
        uint8 resetIn = 0; // always reset there, even at 10 minutes or more
        uint8 neverIn = 0; // never reset there
    };

    // The core's arena rule: cooldowns under 10 minutes, not item cooldowns without spell data, nor the ones that
    // only start when leaving combat
    bool IsShortCooldown(SpellInfo const* spellInfo, SpellCooldown const& cooldown, uint32 infTime)
    {
        if (spellInfo->HasAttribute(SPELL_ATTR4_IGNORE_DEFAULT_ARENA_RESTRICTIONS))
            return true;

        uint32 const limit = 10 * MINUTE * IN_MILLISECONDS;
        return spellInfo->RecoveryTime < limit && spellInfo->CategoryRecoveryTime < limit &&
            cooldown.end < infTime && cooldown.maxduration < limit;
    }

    // `dynamic_scaling_cooldown_reset`.`type`
    enum CooldownEntryType : uint8
    {
        ENTRY_CREATURE = 0, // a creature entry: 1 resets when it dies or wipes the group, 0 never
        ENTRY_CREDIT   = 1  // an encounter, by credit entry or DungeonEncounter.dbc ID: 0 = it doesn't reset
    };

    struct MapResetInfo : public DataMap::Base
    {
        uint32 lastReset = 0; // getMSTime() of the last reset, so one kill doesn't reset twice
    };

    constexpr char const* MAP_RESET_KEY = "DynamicScalingCooldownReset";
    constexpr uint32 RESET_DEDUPE_MS = 3 * IN_MILLISECONDS;
}

class CooldownResetMgr
{
public:
    static CooldownResetMgr* instance()
    {
        static CooldownResetMgr mgr;
        return &mgr;
    }

    void LoadConfig()
    {
        enable = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.Enable", true);
        announce = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.Announce", true);
        useDBOverride = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.UseDBOverride", false);
        for (uint8 i = 0; i < MAX_COOLDOWN_RULE; ++i)
            configRules[i] = sConfigMgr->GetOption<bool>(
                std::string("DynamicScaling.CooldownReset.") + RULE_NAMES[i].name, RULE_NAMES[i].defaultValue);
        combatTimeMs = sConfigMgr->GetOption<uint32>("DynamicScaling.CooldownReset.CombatTimeRequired", 30) *
            IN_MILLISECONDS;
        shortCooldowns = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.ShortCooldowns", true);
        removeSated = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.RemoveSated", true);
        petCooldowns = sConfigMgr->GetOption<bool>("DynamicScaling.CooldownReset.PetCooldowns", false);
    }

    void LoadTables()
    {
        for (uint8 i = 0; i < MAX_COOLDOWN_RULE; ++i)
            dbRules[i] = RULE_NAMES[i].defaultValue;

        if (QueryResult result = WorldDatabase.Query("SELECT `rule`, `reset` FROM `dynamic_scaling_cooldown_rule`"))
        {
            do
            {
                Field* fields = result->Fetch();
                std::string rule = fields[0].Get<std::string>();
                bool found = false;
                for (uint8 i = 0; i < MAX_COOLDOWN_RULE; ++i)
                    if (StringEqualI(rule, RULE_NAMES[i].name))
                    {
                        dbRules[i] = fields[1].Get<uint8>() != 0;
                        found = true;
                    }
                if (!found)
                    LOG_ERROR("module", "mod-dynamic-scaling: unknown rule '{}' in `dynamic_scaling_cooldown_rule`.",
                        rule);
            } while (result->NextRow());
        }

        creatureRules.clear();
        deniedCredits.clear();
        if (QueryResult result = WorldDatabase.Query(
            "SELECT `type`, `entry`, `reset` FROM `dynamic_scaling_cooldown_reset`"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 entry = fields[1].Get<uint32>();
                bool reset = fields[2].Get<uint8>() != 0;
                if (fields[0].Get<uint8>() == ENTRY_CREDIT)
                {
                    if (!reset)
                        deniedCredits[entry] = true;
                }
                else
                    creatureRules[entry] = reset;
            } while (result->NextRow());
        }

        spellRules.clear();
        categoryRules.clear();
        if (QueryResult result = WorldDatabase.Query(
            "SELECT `type`, `id`, `reset_in`, `never_in` FROM `dynamic_scaling_cooldown_spell`"))
        {
            do
            {
                Field* fields = result->Fetch();
                SpellRule rule{ fields[2].Get<uint8>(), fields[3].Get<uint8>() };
                uint32 id = fields[1].Get<uint32>();
                if (fields[0].Get<uint8>() == SPELL_TYPE_CATEGORY)
                    categoryRules[id] = rule;
                else if (sSpellMgr->GetSpellInfo(id))
                    spellRules[id] = rule;
                else
                    LOG_ERROR("module", "mod-dynamic-scaling: spell {} in `dynamic_scaling_cooldown_spell` doesn't "
                        "exist.", id);
            } while (result->NextRow());
        }

        LOG_INFO("module", "mod-dynamic-scaling: cooldown resets {}, {} creature, {} encounter and {} spell rules.",
            enable ? "on" : "off", creatureRules.size(), deniedCredits.size(),
            spellRules.size() + categoryRules.size());
    }

    bool Rule(CooldownRule rule) const { return useDBOverride ? dbRules[rule] : configRules[rule]; }

    // Is this instance kind switched on?
    bool IsMapEnabled(Map const* map) const
    {
        if (!enable || !map || !map->IsDungeon() || !map->GetInstanceId())
            return false;

        if (!map->IsRaid())
            return Rule(map->IsHeroic() ? RULE_DUNGEON_HEROIC : RULE_DUNGEON);

        // 20 and 40 player raids count with the 25 player ones
        MapDifficulty const* difficulty = map->GetMapDifficulty();
        uint32 maxPlayers = difficulty && difficulty->maxPlayers ? difficulty->maxPlayers :
            (map->GetEntry() ? map->GetEntry()->maxPlayers : 25);
        bool tenPlayer = maxPlayers <= 10;
        if (map->IsHeroic())
            return Rule(tenPlayer ? RULE_RAID_10_HEROIC : RULE_RAID_25_HEROIC);
        return Rule(tenPlayer ? RULE_RAID_10 : RULE_RAID_25);
    }

    // Does this creature reset cooldowns when it dies? 1 / 0 = yes / no; -1 = not one of ours
    int8 CreatureRule(Creature const* creature, bool forDeath) const
    {
        auto itr = creatureRules.find(creature->GetEntry());
        if (itr != creatureRules.end())
            return itr->second ? 1 : 0;

        if (!forDeath)
            return -1; // wipes count only for listed creatures and boss encounters (elites could be pulled and reset)

        CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
        if ((creature->IsDungeonBoss() && Rule(RULE_BOSS)) || (creature->isWorldBoss() && Rule(RULE_WORLD_BOSS)))
            return 1;
        bool elite = cInfo->rank == CREATURE_ELITE_ELITE || cInfo->rank == CREATURE_ELITE_RAREELITE;
        bool rare = cInfo->rank == CREATURE_ELITE_RARE || cInfo->rank == CREATURE_ELITE_RAREELITE;
        if ((elite && Rule(RULE_ELITE)) || (rare && Rule(RULE_RARE)))
            return 1;
        return -1;
    }

    bool IsCreditDenied(uint32 creditEntry) const { return deniedCredits.count(creditEntry) > 0; }

    // Is this spell (or its category) always (1) or never (0) reset here? -1 = not listed. The spell's row wins.
    int8 SpellListed(uint32 spellId, uint32 category, CooldownContext context) const
    {
        for (auto const* rules : { &spellRules, &categoryRules })
        {
            uint32 id = rules == &spellRules ? spellId : category;
            if (!id)
                continue;

            auto itr = rules->find(id);
            if (itr == rules->end())
                continue;
            if (itr->second.neverIn & context)
                return 0;
            if (itr->second.resetIn & context)
                return 1;
        }
        return -1;
    }

    // Battlegrounds, arenas and duels
    bool IsEnabled(CooldownRule rule) const { return enable && Rule(rule); }

    // Boss encounter timers (SetBossState), per instance and boss
    void SetBossCombatStarted(uint32 instanceId, uint32 bossId)
    {
        std::lock_guard lock(bossLock);
        bossCombat[{ instanceId, bossId }] = getMSTime();
    }

    bool HasBossFoughtLongEnough(uint32 instanceId, uint32 bossId)
    {
        std::lock_guard lock(bossLock);
        auto itr = bossCombat.find({ instanceId, bossId });
        if (itr == bossCombat.end())
            return false;

        bool enough = GetMSTimeDiffToNow(itr->second) >= combatTimeMs;
        bossCombat.erase(itr);
        return enough;
    }

    // Everyone in an instance
    void ResetCooldowns(Map* map, std::string const& reason)
    {
        MapResetInfo* info = map->CustomData.GetDefault<MapResetInfo>(MAP_RESET_KEY);
        if (info->lastReset && GetMSTimeDiffToNow(info->lastReset) < RESET_DEDUPE_MS)
            return;
        info->lastReset = getMSTime();

        map->DoForAllPlayers([&](Player* player) { ResetPlayer(player, CONTEXT_INSTANCE, reason); });

        LOG_DEBUG("module", "mod-dynamic-scaling: cooldowns reset in {} ({}): {}", map->GetMapName(), map->GetId(),
            reason);
    }

    // One player's cooldowns. startedAfter (game time in ms): only cooldowns that started since then (duels).
    void ResetPlayer(Player* player, CooldownContext context, std::string const& reason, uint32 startedAfter = 0)
    {
        uint32 infTime = GameTime::GetGameTimeMS().count() + infinityCooldownDelayCheck;
        std::vector<uint32> reset;
        for (auto const& [spellId, cooldown] : player->GetSpellCooldownMap())
        {
            if (startedAfter && (cooldown.end < cooldown.maxduration || cooldown.end - cooldown.maxduration <
                startedAfter))
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            int8 listed = SpellListed(spellId, spellInfo->GetCategory(), context);
            if (listed == 1 || (listed == -1 && shortCooldowns && IsShortCooldown(spellInfo, cooldown, infTime)))
                reset.push_back(spellId);
        }

        for (uint32 spellId : reset)
            player->RemoveSpellCooldown(spellId, true);

        // Not after duels: a Bloodlust from before the duel would come back
        if (removeSated && context != CONTEXT_DUEL)
        {
            player->RemoveAura(SPELL_EXHAUSTION);
            player->RemoveAura(SPELL_SATED);
        }

        if (petCooldowns)
            if (Pet* pet = player->GetPet())
            {
                for (auto const& cooldown : pet->m_CreatureSpellCooldowns)
                    player->SendClearCooldown(cooldown.first, pet);
                pet->m_CreatureSpellCooldowns.clear();
                if (removeSated && context != CONTEXT_DUEL)
                {
                    pet->RemoveAura(SPELL_EXHAUSTION);
                    pet->RemoveAura(SPELL_SATED);
                }
            }

        if (announce)
            ChatHandler(player->GetSession()).PSendSysMessage("|cff4CFF00[Dynamic Scaling]|r Cooldowns reset ({}).",
                reason);
    }

    bool enable = true;
    uint32 combatTimeMs = 30 * IN_MILLISECONDS;

private:
    bool announce = true;
    bool useDBOverride = false;
    bool configRules[MAX_COOLDOWN_RULE] = {};
    bool dbRules[MAX_COOLDOWN_RULE] = {};
    bool shortCooldowns = true;
    bool removeSated = true;
    bool petCooldowns = false;
    std::unordered_map<uint32, SpellRule> spellRules;
    std::unordered_map<uint32, SpellRule> categoryRules;
    std::unordered_map<uint32, bool> creatureRules;
    std::unordered_map<uint32, bool> deniedCredits;

    std::mutex bossLock;
    std::map<std::pair<uint32, uint32>, uint32> bossCombat;
};

#define sCooldownReset CooldownResetMgr::instance()

void LoadDynamicScalingCooldownTables()
{
    sCooldownReset->LoadTables();
}

namespace
{
    struct CreatureCombatInfo : public DataMap::Base
    {
        uint32 combatStart = 0;
    };

    constexpr char const* CREATURE_COMBAT_KEY = "DynamicScalingCooldownCombat";
}

class DynamicScalingCooldownWorldScript : public WorldScript
{
public:
    DynamicScalingCooldownWorldScript() : WorldScript("DynamicScalingCooldownWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool /*reload*/) override { sCooldownReset->LoadConfig(); }
    void OnBeforeWorldInitialized() override { sCooldownReset->LoadTables(); }
};

class DynamicScalingCooldownGlobalScript : public GlobalScript
{
public:
    DynamicScalingCooldownGlobalScript() : GlobalScript("DynamicScalingCooldownGlobalScript",
        { GLOBALHOOK_ON_AFTER_UPDATE_ENCOUNTER_STATE, GLOBALHOOK_ON_BEFORE_SET_BOSS_STATE }) { }

    // A boss encounter completed
    void OnAfterUpdateEncounterState(Map* map, EncounterCreditType type, uint32 creditEntry, Unit* /*source*/,
        Difficulty /*difficulty*/, std::list<DungeonEncounter const*> const* encounters,
        uint32 /*dungeonCompleted*/, bool /*updated*/) override
    {
        if (!sCooldownReset->IsMapEnabled(map) || !sCooldownReset->Rule(RULE_BOSS) ||
            sCooldownReset->IsCreditDenied(creditEntry))
            return;

        // The deny list may also name the encounter by its DungeonEncounter.dbc ID
        if (encounters)
            for (DungeonEncounter const* encounter : *encounters)
                if (encounter && encounter->creditType == type && encounter->creditEntry == creditEntry &&
                    encounter->dbcEntry && sCooldownReset->IsCreditDenied(encounter->dbcEntry->id))
                    return;

        sCooldownReset->ResetCooldowns(map, "boss defeated");
    }

    // A boss fight started, or the group wiped after fighting it long enough
    void OnBeforeSetBossState(uint32 bossId, EncounterState newState, EncounterState oldState, Map* map) override
    {
        if (oldState == newState || !sCooldownReset->IsMapEnabled(map) || !sCooldownReset->Rule(RULE_BOSS))
            return;

        if (newState == IN_PROGRESS)
        {
            sCooldownReset->SetBossCombatStarted(map->GetInstanceId(), bossId);
            return;
        }

        // DONE comes through OnAfterUpdateEncounterState
        if (oldState != IN_PROGRESS || (newState != FAIL && newState != NOT_STARTED))
            return;

        if (sCooldownReset->HasBossFoughtLongEnough(map->GetInstanceId(), bossId))
            sCooldownReset->ResetCooldowns(map, "boss reset");
    }
};

class DynamicScalingCooldownUnitScript : public UnitScript
{
public:
    DynamicScalingCooldownUnitScript() : UnitScript("DynamicScalingCooldownUnitScript", true, {
        UNITHOOK_ON_UNIT_ENTER_COMBAT, UNITHOOK_ON_UNIT_ENTER_EVADE_MODE, UNITHOOK_ON_UNIT_DEATH }) { }

    void OnUnitEnterCombat(Unit* unit, Unit* /*victim*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (creature && sCooldownReset->IsMapEnabled(creature->GetMap()))
            creature->CustomData.GetDefault<CreatureCombatInfo>(CREATURE_COMBAT_KEY)->combatStart = getMSTime();
    }

    // A listed creature (one without a boss encounter) reset after a long enough fight
    void OnUnitEnterEvadeMode(Unit* unit, uint8 evadeReason) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || !sCooldownReset->IsMapEnabled(creature->GetMap()))
            return;

        // Running it out of its area, or out of reach, doesn't count
        if (evadeReason == CreatureAI::EVADE_REASON_BOUNDARY || evadeReason == CreatureAI::EVADE_REASON_NO_PATH)
            return;

        CreatureCombatInfo* info = creature->CustomData.Get<CreatureCombatInfo>(CREATURE_COMBAT_KEY);
        if (!info || !info->combatStart || sCooldownReset->CreatureRule(creature, false) != 1)
            return;

        bool enough = GetMSTimeDiffToNow(info->combatStart) >= sCooldownReset->combatTimeMs;
        info->combatStart = 0;
        if (enough)
            sCooldownReset->ResetCooldowns(creature->GetMap(), creature->GetName() + " reset");
    }

    // A boss, world boss, elite or rare (as switched on), or a listed creature, died
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!creature || creature->IsPet() || creature->IsGuardian() || creature->GetCharmerOrOwnerGUID().IsPlayer() ||
            !sCooldownReset->IsMapEnabled(creature->GetMap()))
            return;

        // Summoned adds only count when listed, or when they are the boss
        int8 rule = sCooldownReset->CreatureRule(creature, true);
        if (creature->IsSummon() && rule == 1 && !creature->IsDungeonBoss() &&
            sCooldownReset->CreatureRule(creature, false) != 1)
            return;

        if (rule == 1)
            sCooldownReset->ResetCooldowns(creature->GetMap(), creature->GetName() + " defeated");
    }
};

namespace
{
    struct PlayerDuelInfo : public DataMap::Base
    {
        uint32 start = 0; // game time in ms, with the countdown
    };

    constexpr char const* PLAYER_DUEL_KEY = "DynamicScalingCooldownDuel";
    constexpr uint32 DUEL_COUNTDOWN_MS = 3 * IN_MILLISECONDS;
}

class DynamicScalingCooldownBattlegroundScript : public AllBattlegroundScript
{
public:
    DynamicScalingCooldownBattlegroundScript() : AllBattlegroundScript("DynamicScalingCooldownBattlegroundScript",
        { ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_START, ALLBATTLEGROUNDHOOK_ON_BATTLEGROUND_ADD_PLAYER }) { }

    // The gates of a battleground (not an arena) open
    void OnBattlegroundStart(Battleground* bg) override
    {
        if (bg->isArena() || !sCooldownReset->IsEnabled(RULE_BATTLEGROUND))
            return;

        for (auto const& [guid, player] : bg->GetPlayers())
            if (player)
                sCooldownReset->ResetPlayer(player, CONTEXT_BATTLEGROUND, "the battle begins");
    }

    // Entering an arena (after the core's own reset), or a battleground already under way
    void OnBattlegroundAddPlayer(Battleground* bg, Player* player) override
    {
        if (bg->isArena())
        {
            if (sCooldownReset->IsEnabled(RULE_ARENA))
                sCooldownReset->ResetPlayer(player, CONTEXT_ARENA, "arena");
        }
        else if (bg->GetStatus() == STATUS_IN_PROGRESS && sCooldownReset->IsEnabled(RULE_BATTLEGROUND))
            sCooldownReset->ResetPlayer(player, CONTEXT_BATTLEGROUND, "joined the battle");
    }
};

class DynamicScalingCooldownPlayerScript : public PlayerScript
{
public:
    DynamicScalingCooldownPlayerScript() : PlayerScript("DynamicScalingCooldownPlayerScript",
        { PLAYERHOOK_ON_DUEL_START, PLAYERHOOK_ON_DUEL_END }) { }

    void OnPlayerDuelStart(Player* player1, Player* player2) override
    {
        if (!sCooldownReset->IsEnabled(RULE_DUEL))
            return;

        uint32 now = GameTime::GetGameTimeMS().count();
        uint32 start = now > DUEL_COUNTDOWN_MS ? now - DUEL_COUNTDOWN_MS : 0;
        for (Player* player : { player1, player2 })
            if (player)
                player->CustomData.GetDefault<PlayerDuelInfo>(PLAYER_DUEL_KEY)->start = start;
    }

    // Only the cooldowns used in the duel, so a duel can't reset cooldowns from before it
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType /*type*/) override
    {
        for (Player* player : { winner, loser })
        {
            if (!player)
                continue;

            PlayerDuelInfo* info = player->CustomData.Get<PlayerDuelInfo>(PLAYER_DUEL_KEY);
            if (!info || !info->start)
                continue;

            uint32 start = info->start;
            info->start = 0;
            if (sCooldownReset->IsEnabled(RULE_DUEL))
                sCooldownReset->ResetPlayer(player, CONTEXT_DUEL, "duel over", start);
        }
    }
};

void AddDynamicScalingCooldownScripts()
{
    new DynamicScalingCooldownBattlegroundScript();
    new DynamicScalingCooldownPlayerScript();
    new DynamicScalingCooldownWorldScript();
    new DynamicScalingCooldownGlobalScript();
    new DynamicScalingCooldownUnitScript();
}
