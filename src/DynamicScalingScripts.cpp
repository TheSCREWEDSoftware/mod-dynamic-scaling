/*
 * mod-dynamic-scaling: script hooks.
 */

#include "DynamicScaling.h"

#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "World.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    constexpr char const* CREATURE_INFO_KEY = "DynamicScalingCreatureInfo";

    DSCreatureInfo* GetInfo(Creature* creature)
    {
        return creature->CustomData.GetDefault<DSCreatureInfo>(CREATURE_INFO_KEY);
    }

    DSCreatureInfo* FindInfo(Unit const* unit)
    {
        if (!unit || !unit->IsCreature())
            return nullptr;

        return const_cast<Unit*>(unit)->ToCreature()->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY);
    }

    bool IsWorldMap(Map const* map)
    {
        return map && !map->Instanceable();
    }

    // Quest money to scale, set when the quest's XP is computed (just before its money is given)
    struct QuestMoneyScale
    {
        float factor = 1.0f;
        int64 time = 0;
    };
    thread_local std::unordered_map<ObjectGuid::LowType, QuestMoneyScale> PendingQuestMoney;

    // Set right before the core checks whether a quest item in a loot may be shown to a player
    thread_local bool CheckingQuestLootItem = false;

    // Players waiting for a dungeon respawn after releasing their spirit
    std::unordered_set<ObjectGuid::LowType> PendingRespawns;
    std::mutex PendingRespawnsLock;

    // Takes the player off the list; true if they were on it
    bool TakePendingRespawn(ObjectGuid::LowType guid)
    {
        std::lock_guard lock(PendingRespawnsLock);
        return PendingRespawns.erase(guid) > 0;
    }
}

class DynamicScalingWorldScript : public WorldScript
{
public:
    DynamicScalingWorldScript() : WorldScript("DynamicScalingWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_STARTUP }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        sDynamicScaling->LoadConfig();
        if (reload)
        {
            sDynamicScaling->ApplyEntryLevelOverrides();
            sDynamicScaling->ApplyQuestMinLevelFallback();
        }
    }

    void OnBeforeWorldInitialized() override
    {
        sDynamicScaling->LoadTables();
        sDynamicScaling->ApplyEntryLevelOverrides();
    }

    // After every module's OnBeforeWorldInitialized, so mod-world-randomizer has written its dungeon brackets
    void OnStartup() override
    {
        sDynamicScaling->LoadCollective();
        sDynamicScaling->ApplyQuestMinLevelFallback();
    }
};

class DynamicScalingCreatureScript : public AllCreatureScript
{
public:
    DynamicScalingCreatureScript() : AllCreatureScript("DynamicScalingCreatureScript") { }

    void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature, uint8& level) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable)
            return;

        DSCreatureInfo* info = GetInfo(creature);

        // A rescale asked for this exact level
        if (info->pendingLevel)
        {
            if (!info->originalLevel)
                info->originalLevel = level;

            level = info->pendingLevel;
            info->scaledLevel = info->pendingLevel == info->originalLevel ? 0 : info->pendingLevel;
            info->pendingLevel = 0;
            return;
        }

        // Spawn or respawn: remember the template roll as the creature's own level
        info->originalLevel = level;
        info->scaledLevel = 0;
        info->goldScaled = false;

        info->worldMode = ScalingMode::Off;

        Map* map = creature->GetMap();
        if (map && IsWorldMap(map))
        {
            SpawnAtWorldLevel(creature, info, map, level);
            return;
        }

        if (!map || !map->IsDungeon() || !sDynamicScaling->IsCreatureEligible(creature, false))
            return;

        InstanceSettings settings;
        if (!sDynamicScaling->GetInstanceSettings(map, settings))
            return;

        DSMapInfo* mapInfo = sDynamicScaling->GetMapInfo(map);
        uint8 target = sDynamicScaling->ComputeInstanceLevel(creature, settings, mapInfo, level);
        if (target != level)
        {
            level = target;
            info->scaledLevel = target;
        }

        info->mapVersion = mapInfo ? mapInfo->version : 0;
        info->settingsVersion = sDynamicScaling->GetSettingsVersion();
    }

    void OnCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* creature) override
    {
        if (!sDynamicScaling->Config().enable)
            return;

        DSCreatureInfo* info = creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY);
        if (!info)
            return;

        sDynamicScaling->ComputeMultipliers(creature, info);

        if (info->healthMult != 1.0f)
        {
            uint32 health = std::max<uint32>(1, uint32(float(creature->GetMaxHealth()) * info->healthMult));
            creature->SetCreateHealth(health);
            creature->SetStatFlatModifier(UNIT_MOD_HEALTH, BASE_VALUE, float(health));
            creature->SetMaxHealth(health);
            creature->SetHealth(health);
            creature->ResetPlayerDamageReq();
        }

        if (info->manaMult != 1.0f && creature->GetMaxPower(POWER_MANA))
        {
            uint32 mana = uint32(float(creature->GetMaxPower(POWER_MANA)) * info->manaMult);
            creature->SetCreateMana(mana);
            creature->SetStatFlatModifier(UNIT_MOD_MANA, BASE_VALUE, float(mana));
            creature->SetMaxPower(POWER_MANA, mana);
            creature->SetPower(POWER_MANA, mana);
        }
    }

    void OnCreatureAddWorld(Creature* creature) override
    {
        // UpdateEntry() sets armor after SelectLevel(), so the multiplier goes on here
        DSCreatureInfo* info = creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY);
        if (!info || info->armorMult == 1.0f)
            return;

        sDynamicScaling->ApplyArmor(creature, info);
        creature->UpdateArmor();
    }

    void OnAllCreatureUpdate(Creature* creature, uint32 diff) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable)
            return;

        DSCreatureInfo* info = GetInfo(creature);
        info->cooldown -= int32(diff);
        info->updateTimer -= int32(diff);
        if (info->updateTimer > 0)
            return;

        info->updateTimer = int32(config.worldUpdateInterval);

        if (!creature->IsAlive() || creature->IsInCombat() || creature->IsInEvadeMode())
            return;

        Map* map = creature->GetMap();
        if (!map)
            return;

        if (map->IsDungeon())
            UpdateInstanceCreature(creature, info, map);
        else if (IsWorldMap(map))
            UpdateWorldCreature(creature, info);
    }

private:
    // Spawns, respawns and summons come in already at the level of the nearest player, so nobody sees them change
    void SpawnAtWorldLevel(Creature* creature, DSCreatureInfo* info, Map* map, uint8& level)
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.worldEnable || !sDynamicScaling->IsCreatureEligible(creature, true))
            return;

        float range = sDynamicScaling->GetWorldRange(map);
        Player* nearest = nullptr;
        float nearestDist = range * range;
        for (auto const& ref : map->GetPlayers())
        {
            Player* player = ref.GetSource();
            if (!player || player->IsGameMaster() || player->IsInFlight() || !sDynamicScaling->IsScalingFor(player))
                continue;

            float dist = player->GetExactDist2dSq(creature->GetPositionX(), creature->GetPositionY());
            if (dist <= nearestDist)
            {
                nearest = player;
                nearestDist = dist;
            }
        }

        if (!nearest)
            return;

        uint32 zoneId = 0;
        uint32 areaId = 0;
        map->GetZoneAndAreaId(creature->GetPhaseMask(), zoneId, areaId, creature->GetPositionX(),
            creature->GetPositionY(), creature->GetPositionZ());

        ScalingMode mode = sDynamicScaling->GetPlayerMode(nearest);
        ZoneSettings settings;
        if (!sDynamicScaling->GetZoneSettings(map->GetId(), zoneId, areaId, settings, mode))
            return;

        uint8 target = sDynamicScaling->ComputeWorldLevel(creature, settings, nearest->GetLevel(), level);
        if (target == level)
            return;

        level = target;
        info->scaledLevel = target;
        info->worldMode = mode;
        info->cooldown = int32(config.worldRescaleCooldown);
    }

    void UpdateInstanceCreature(Creature* creature, DSCreatureInfo* info, Map* map)
    {
        DSMapInfo* mapInfo = sDynamicScaling->GetMapInfo(map);
        uint32 mapVersion = mapInfo ? mapInfo->version : 0;
        if (info->mapVersion == mapVersion && info->settingsVersion == sDynamicScaling->GetSettingsVersion())
            return;

        info->mapVersion = mapVersion;
        info->settingsVersion = sDynamicScaling->GetSettingsVersion();

        if (!info->originalLevel || !sDynamicScaling->IsCreatureEligible(creature, false))
            return;

        InstanceSettings settings;
        uint8 target = info->originalLevel;
        if (sDynamicScaling->GetInstanceSettings(map, settings))
            target = sDynamicScaling->ComputeInstanceLevel(creature, settings, mapInfo, info->originalLevel);
        else if (!info->scaledLevel && info->healthMult == 1.0f && info->damageMult == 1.0f)
            return;

        // Re-run even at the same level: the player count (and so the multipliers) may have changed
        sDynamicScaling->RescaleCreature(creature, target);
    }

    void UpdateWorldCreature(Creature* creature, DSCreatureInfo* info)
    {
        DSConfig const& config = sDynamicScaling->Config();

        bool settingsChanged = info->settingsVersion != sDynamicScaling->GetSettingsVersion();
        info->settingsVersion = sDynamicScaling->GetSettingsVersion();

        // Is this place scaled at all (for either mode)?
        ZoneSettings settings;
        if (!sDynamicScaling->GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(),
            settings, ScalingMode::Advanced) ||
            !sDynamicScaling->IsCreatureEligible(creature, true))
        {
            if (info->scaledLevel)
            {
                sDynamicScaling->ResetCreature(creature);
                info->worldMode = ScalingMode::Off;
            }
            return;
        }

        if (info->cooldown > 0 && !settingsChanged)
            return;

        if (!info->originalLevel)
            info->originalLevel = creature->GetLevel();

        bool onlyOptedOut = false;
        ScalingMode mode = ScalingMode::Off;
        uint8 reference = GetWorldReferenceLevel(creature, config, onlyOptedOut, mode);
        if (!reference)
        {
            // Only players who turned scaling off are watching: show them the real level
            if (onlyOptedOut && info->scaledLevel)
            {
                sDynamicScaling->ResetCreature(creature);
                info->worldMode = ScalingMode::Off;
                info->cooldown = int32(config.worldRescaleCooldown);
            }
            return;
        }

        // The nearest (reference) player's choice decides: Simple trails anywhere, Advanced keeps the window
        sDynamicScaling->GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings,
            mode);
        uint8 target = sDynamicScaling->ComputeWorldLevel(creature, settings, reference, info->originalLevel);
        if (target == creature->GetLevel() && mode == info->worldMode && !settingsChanged)
            return;

        info->worldMode = mode;
        sDynamicScaling->RescaleCreature(creature, target);
        info->cooldown = int32(config.worldRescaleCooldown);
    }

    uint8 GetWorldReferenceLevel(Creature* creature, DSConfig const& config, bool& onlyOptedOut,
        ScalingMode& mode)
    {
        onlyOptedOut = false;
        float range = sDynamicScaling->GetWorldRange(creature->GetMap());
        std::list<Player*> players;
        Acore::AnyPlayerInObjectRangeCheck check(creature, range, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(creature, players, check);
        Cell::VisitObjects(creature, searcher, range);

        Player* chosen = nullptr;
        float chosenDist = 0.0f;
        for (Player* player : players)
        {
            if (!player || player->IsGameMaster() || player->IsInFlight() || !sDynamicScaling->IsScalingFor(player))
                continue;

            if (!chosen)
            {
                chosen = player;
                chosenDist = creature->GetExactDist2dSq(player);
                continue;
            }

            switch (config.worldReference)
            {
                case LevelReference::Highest:
                    if (player->GetLevel() > chosen->GetLevel())
                        chosen = player;
                    break;
                case LevelReference::Lowest:
                case LevelReference::Average:
                    if (player->GetLevel() < chosen->GetLevel())
                        chosen = player;
                    break;
                default:
                {
                    float dist = creature->GetExactDist2dSq(player);
                    if (dist < chosenDist)
                    {
                        chosen = player;
                        chosenDist = dist;
                    }
                    break;
                }
            }
        }

        if (!chosen)
        {
            onlyOptedOut = std::any_of(players.begin(), players.end(), [](Player* player)
            {
                return player && !player->IsGameMaster() && !sDynamicScaling->IsScalingFor(player);
            });
            return 0;
        }

        mode = sDynamicScaling->GetPlayerMode(chosen);
        if (!config.worldGroupAverage)
            return chosen->GetLevel();

        Group* group = chosen->GetGroup();
        if (!group)
            return chosen->GetLevel();

        // The group's average level, counting only members who are close by
        uint32 sum = 0;
        uint32 count = 0;
        for (Player* player : players)
        {
            if (player && player->GetGroup() == group && !player->IsGameMaster() &&
                sDynamicScaling->IsScalingFor(player))
            {
                sum += player->GetLevel();
                ++count;
            }
        }

        return count ? uint8((sum + count / 2) / count) : chosen->GetLevel();
    }
};

class DynamicScalingUnitScript : public UnitScript
{
    uint8 GroupLevel(Player* player, Creature* creature, DSConfig const& config)
    {
        Group* group = config.worldGroupAverage ? player->GetGroup() : nullptr;
        if (!group)
            return player->GetLevel();

        float range = sDynamicScaling->GetWorldRange(creature->GetMap());
        uint32 sum = 0;
        uint32 count = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->GetMap() == creature->GetMap() && !member->IsGameMaster() &&
                    sDynamicScaling->IsScalingFor(member) && member->IsWithinDistInMap(creature, range))
                {
                    sum += member->GetLevel();
                    ++count;
                }

        return count ? uint8((sum + count / 2) / count) : player->GetLevel();
    }

public:
    DynamicScalingUnitScript() : UnitScript("DynamicScalingUnitScript", true, {
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK,
        UNITHOOK_ON_UNIT_ENTER_COMBAT
    }) { }

    // Open world: the creature takes the level of whoever it engaged (their group's average) for the whole fight
    void OnUnitEnterCombat(Unit* unit, Unit* victim) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        Creature* creature = unit ? unit->ToCreature() : nullptr;
        if (!config.enable || !config.worldEnable || !config.worldScaleOnEngage || !creature || !victim ||
            !IsWorldMap(creature->GetMap()))
            return;

        Player* player = victim->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!player || player->IsGameMaster())
            return;

        DSCreatureInfo* info = GetInfo(creature);
        if (!info->originalLevel)
            info->originalLevel = creature->GetLevel();

        ScalingMode mode = sDynamicScaling->GetPlayerMode(player);
        ZoneSettings settings;
        uint8 target = info->originalLevel;
        if (mode != ScalingMode::Off && sDynamicScaling->IsCreatureEligible(creature, true) &&
            sDynamicScaling->GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(),
            settings, mode))
            target = sDynamicScaling->ComputeWorldLevel(creature, settings, GroupLevel(player, creature, config),
                info->originalLevel);
        else
            mode = ScalingMode::Off;

        if (target == creature->GetLevel())
            return;

        float healthPct = creature->GetHealthPct();
        info->worldMode = mode;
        sDynamicScaling->RescaleCreature(creature, target, false);
        creature->SetHealth(std::max<uint32>(1, creature->CountPctFromMaxHealth(healthPct)));
    }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        if (DSCreatureInfo const* info = FindInfo(attacker))
            if (info->damageMult != 1.0f)
                damage = uint32(float(damage) * info->damageMult);
    }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage,
        SpellInfo const* /*spellInfo*/) override
    {
        if (damage <= 0)
            return;

        if (DSCreatureInfo const* info = FindInfo(attacker))
            if (info->damageMult != 1.0f)
                damage = int32(float(damage) * info->damageMult);
    }

    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage,
        SpellInfo const* /*spellInfo*/) override
    {
        if (DSCreatureInfo const* info = FindInfo(attacker))
            if (info->damageMult != 1.0f)
                damage = uint32(float(damage) * info->damageMult);
    }
};

class DynamicScalingMapScript : public AllMapScript
{
public:
    DynamicScalingMapScript() : AllMapScript("DynamicScalingMapScript",
        { ALLMAPHOOK_ON_PLAYER_ENTER_ALL, ALLMAPHOOK_ON_PLAYER_LEAVE_ALL }) { }

    void OnPlayerEnterAll(Map* map, Player* player) override
    {
        Refresh(map, player);
    }

    void OnPlayerLeaveAll(Map* map, Player* player) override
    {
        Refresh(map, player);
    }

private:
    // The group size changed for everyone else inside, so their Simple mode buff changes too
    void Refresh(Map* map, Player* changed)
    {
        if (!map || !map->IsDungeon())
            return;

        sDynamicScaling->RefreshMapInfo(map);

        for (auto const& ref : map->GetPlayers())
            if (Player* player = ref.GetSource())
                if (player != changed && player->IsInWorld())
                    sDynamicScaling->UpdatePlayerBuff(player);
    }
};

class DynamicScalingPlayerScript : public PlayerScript
{
public:
    DynamicScalingPlayerScript() : PlayerScript("DynamicScalingPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_LEVEL_CHANGED,
        PLAYERHOOK_ON_PLAYER_RELEASED_GHOST,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_ON_GIVE_EXP,
        PLAYERHOOK_ON_CREATURE_KILL,
        PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
        PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
        PLAYERHOOK_ON_MONEY_CHANGED,
        PLAYERHOOK_ON_BEFORE_LOOT_MONEY,
        PLAYERHOOK_PASSED_QUEST_KILLED_MONSTER_CREDIT,
#if DS_HAS_QUEST_MIN_LEVEL_HOOK
        PLAYERHOOK_ON_GET_QUEST_MIN_LEVEL,
#endif
        PLAYERHOOK_ON_BEFORE_FILL_QUEST_LOOT_ITEM
    }) { }

#if DS_HAS_QUEST_MIN_LEVEL_HOOK
    // Quests from givers in a scaled place: Simple has no level requirement, Advanced the zone's lowest level
    void OnPlayerGetQuestMinLevel(Player const* player, Quest const* /*quest*/, uint32& minLevel) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.worldEnable || !config.questMinLevel || minLevel <= 1 ||
            !IsWorldMap(player->GetMap()))
            return;

        ScalingMode mode = sDynamicScaling->GetPlayerMode(player);
        ZoneSettings settings;
        if (mode == ScalingMode::Off || !sDynamicScaling->GetZoneSettings(player->GetMapId(), player->GetZoneId(),
            player->GetAreaId(), settings, mode))
            return;

        if (mode == ScalingMode::Simple)
            minLevel = 1;
        else if (settings.levelMin)
            minLevel = std::min<uint32>(minLevel, settings.levelMin);
    }
#endif

    // Scaling off: kills far below you give no quest progress
    bool OnPlayerPassedQuestKilledMonsterCredit(Player* player, Quest const* /*quest*/, uint32 /*entry*/,
        uint32 /*realEntry*/, ObjectGuid guid) override
    {
        return !sDynamicScaling->BlocksQuestCredit(player, guid);
    }

    // ...and no quest items: the next loot check (the global script below) is for this quest item
    void OnPlayerBeforeFillQuestLootItem(Player* /*player*/, LootItem& /*item*/) override
    {
        CheckingQuestLootItem = true;
    }

    void OnPlayerLogin(Player* player) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        sDynamicScaling->LoadPlayerChoice(player);

        if (config.enable && config.announce)
        {
            ChatHandler handler(player->GetSession());
            handler.SendSysMessage("This server is running the |cff4CFF00Dynamic Scaling|r module.");
            if (config.playerToggle)
                handler.PSendSysMessage("Your scaling: |cff4CFF00{}|r. Type |cffFFFF00.scaling simple|r, "
                    "|cffFFFF00.scaling advanced|r or |cffFFFF00.scaling off|r to change it.",
                    DynamicScalingMgr::ModeName(sDynamicScaling->GetPlayerMode(player)));
        }

        sDynamicScaling->UpdatePlayerBuff(player);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        // Before SaveToDB, so the buff aura is never saved with the character
        sDynamicScaling->RemovePlayerBuff(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        TakePendingRespawn(player->GetGUID().GetCounter());
        sDynamicScaling->ForgetPlayer(player);
    }

    void OnPlayerMapChanged(Player* player) override
    {
        sDynamicScaling->RecordEntryPosition(player);
        sDynamicScaling->UpdatePlayerBuff(player);
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        Map* map = player->GetMap();
        if (map && map->IsDungeon())
        {
            sDynamicScaling->RefreshMapInfo(map);
            sDynamicScaling->UpdatePlayerBuff(player);
        }
    }

    void OnPlayerReleasedGhost(Player* player) override
    {
        InstanceSettings settings;
        if (sDynamicScaling->GetInstanceSettings(player->GetMap(), settings) && settings.respawn)
        {
            std::lock_guard lock(PendingRespawnsLock);
            PendingRespawns.insert(player->GetGUID().GetCounter());
        }
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 /*mapId*/, float /*x*/, float /*y*/, float /*z*/,
        float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        // Taken off first: the TeleportTo() below comes back through this hook
        if (!TakePendingRespawn(player->GetGUID().GetCounter()))
            return true;

        if (player->IsAlive())
            return true;

        InstanceSettings settings;
        Position pos;
        if (!sDynamicScaling->GetInstanceSettings(player->GetMap(), settings) || !settings.respawn ||
            !sDynamicScaling->ResolveRespawnPosition(player, settings, pos))
            return true;

        player->TeleportTo(player->GetMapId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
            pos.GetOrientation());
        player->ResurrectPlayer(sDynamicScaling->Config().respawnHealthPct / 100.0f, false);
        player->SpawnCorpseBones();
        return false;
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* /*victim*/, uint8 /*xpSource*/) override
    {
        if (!sDynamicScaling->Config().simpleXPPenalty)
            return;

        Map* map = player->GetMap();
        if (map && map->IsDungeon())
            amount = uint32(float(amount) * sDynamicScaling->GetPlayerXPModifier(player));
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        AwardMinimumXP(killer, killed);
    }

    void OnPlayerCreatureKilledByPet(Player* owner, Creature* killed) override
    {
        AwardMinimumXP(owner, killed);
    }

    void OnPlayerQuestComputeXP(Player* player, Quest const* quest, uint32& xpValue) override
    {
        PendingQuestMoney.erase(player->GetGUID().GetCounter());

        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.worldEnable || quest->GetQuestLevel() <= 0 ||
            !sDynamicScaling->IsScalingFor(player))
            return;

        if (config.questXPOnlyScaledZones)
        {
            uint32 zoneId = player->GetZoneId();
            uint32 mapId = player->GetMapId();
            if (quest->GetZoneOrSort() > 0)
            {
                zoneId = uint32(quest->GetZoneOrSort());
                if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
                    mapId = area->mapid;
            }

            ZoneSettings settings;
            if (!sDynamicScaling->GetZoneSettings(mapId, zoneId, 0, settings, sDynamicScaling->GetPlayerMode(player)))
                return;
        }

        // The quest counts as if it were between (your level - trail) and your level
        int32 playerLevel = int32(player->GetLevel());
        int32 questLevel = quest->GetQuestLevel();
        int32 floorLevel = (config.questXPEnable && config.questXPMinPct) ? playerLevel - int32(config.questXPTrail) :
            questLevel;
        int32 effectiveLevel = std::max(questLevel, floorLevel);
        if (config.questXPCap)
            effectiveLevel = std::min(effectiveLevel, playerLevel + int32(config.rewardMarginUp));
        effectiveLevel = std::max(effectiveLevel, 1);

        // Fixed reward money (not already following the player's level) shrinks with the quest
        if (config.questMoneyScale && effectiveLevel < questLevel && quest->GetRewOrReqMoney(player->GetLevel()) > 0 &&
            quest->GetRewOrReqMoney(player->GetLevel()) == quest->GetRewOrReqMoney(0))
            PendingQuestMoney[player->GetGUID().GetCounter()] = {
                DynamicScalingMgr::MoneyForLevel(uint8(effectiveLevel)) /
                DynamicScalingMgr::MoneyForLevel(uint8(std::min(questLevel, 80))),
                GameTime::GetGameTimeMS().count() };

        // 0 means "no XP on purpose" (already rewarded, or a no-XP quest)
        QuestXPEntry const* entry = sQuestXPStore.LookupEntry(effectiveLevel);
        if (!xpValue || effectiveLevel == questLevel || !entry || quest->GetXPId() >= 10)
            return;

        // Same math as Quest::XPValue(), at the effective level
        int32 diffFactor = std::clamp<int32>(2 * (effectiveLevel - playerLevel) + 20, 1, 10);
        float xp = float(diffFactor * entry->Exp[quest->GetXPId()] / 10);
        xp *= player->GetQuestRate(quest->IsDFQuest());
        xp *= player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_QUEST_PCT);

        if (effectiveLevel > questLevel) // the trailing floor, a share of a quest at that level
            xpValue = std::max(xpValue, uint32(xp * float(config.questXPMinPct) / 100.0f));
        else                             // far above your level: no more than a quest MarginUp above you
            xpValue = std::min(xpValue, uint32(xp));
    }

    // The quest's reward money comes right after its XP
    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        auto itr = PendingQuestMoney.find(player->GetGUID().GetCounter());
        if (itr == PendingQuestMoney.end())
            return;

        if (amount > 0 && itr->second.time == GameTime::GetGameTimeMS().count())
            amount = std::max<int32>(1, int32(float(amount) * itr->second.factor));
        PendingQuestMoney.erase(itr);
    }

    // A creature's gold follows its scaled level
    void OnPlayerBeforeLootMoney(Player* player, Loot* loot) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.lootMoneyScale || !loot || !loot->gold)
            return;

        Creature* creature = ObjectAccessor::GetCreature(*player, player->GetLootGUID());
        DSCreatureInfo* info = creature ? creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY) : nullptr;
        if (!info || info->goldScaled || !sDynamicScaling->HasScaledLoot(info))
            return;

        info->goldScaled = true;
        float factor = DynamicScalingMgr::MoneyForLevel(info->scaledLevel) /
            DynamicScalingMgr::MoneyForLevel(info->originalLevel);
        loot->gold = std::max<uint32>(1, uint32(float(loot->gold) * factor));
    }

private:
    // Gray kills normally give nothing; give a share of what a trailing-level kill would
    void AwardMinimumXP(Player* killer, Creature* killed)
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.worldEnable || !config.xpAlwaysAward || !killer || !killed)
            return;

        if (killed->IsTotem() || killed->IsPet() || killed->IsCritter() ||
            killed->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_XP) || killed->GetCharmerOrOwnerGUID().IsPlayer())
            return;

        Map* map = killed->GetMap();
        if (!IsWorldMap(map))
            return;

        std::vector<Player*> receivers;
        if (Group* group = killer->GetGroup())
        {
            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
                if (Player* member = itr->GetSource())
                    if (member->IsAlive() && member->GetMap() == map &&
                        member->IsWithinDistInMap(killed, sWorld->getFloatConfig(CONFIG_GROUP_XP_DISTANCE)))
                        receivers.push_back(member);
        }
        else
            receivers.push_back(killer);

        for (Player* player : receivers)
        {
            // Only fill the gap: anyone who already got XP for this kill is skipped
            if (!sDynamicScaling->IsScalingFor(player) || Acore::XP::Gain(player, killed) != 0)
                continue;

            // Each player's own choice: Simple or Advanced settings for this place
            ZoneSettings settings;
            bool scaled = sDynamicScaling->GetZoneSettings(killed->GetMapId(), killed->GetZoneId(),
                killed->GetAreaId(), settings, sDynamicScaling->GetPlayerMode(player));
            if (!scaled && config.xpOnlyScaledZones)
                continue;

            uint32 minPct = scaled ? settings.xpMinPct : config.xpMinPct;
            uint8 trail = scaled ? settings.trailLevels : config.worldTrailLevels;
            if (!minPct)
                continue;

            uint8 level = player->GetLevel();
            uint8 trailingLevel = DynamicScalingMgr::ClampLevel(int32(level) - int32(trail));
            uint32 xp = Acore::XP::BaseGain(level, trailingLevel,
                GetContentLevelsForMapAndZone(killed->GetMapId(), killed->GetZoneId()));
            xp = uint32(float(xp) * sWorld->getRate(RATE_XP_KILL) * float(minPct) / 100.0f);
            xp = uint32(float(xp) * player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT));
            if (receivers.size() > 1)
                xp /= uint32(receivers.size());

            // Through the XP hook like core kill XP, so other modules (XP rates) see it too
            sScriptMgr->OnPlayerGiveXP(player, xp, killed, PlayerXPSource::XPSOURCE_KILL);
            if (xp)
                player->GiveXP(xp, killed);
        }
    }
};

class DynamicScalingGroupScript : public GroupScript
{
public:
    DynamicScalingGroupScript() : GroupScript("DynamicScalingGroupScript",
        { GROUPHOOK_ON_ADD_MEMBER, GROUPHOOK_ON_REMOVE_MEMBER }) { }

    void OnAddMember(Group* group, ObjectGuid /*guid*/) override
    {
        RefreshGroup(group);
    }

    void OnRemoveMember(Group* group, ObjectGuid guid, RemoveMethod /*method*/, ObjectGuid /*kicker*/,
        char const* /*reason*/) override
    {
        RefreshGroup(group);
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            sDynamicScaling->UpdatePlayerBuff(player);
    }

private:
    void RefreshGroup(Group* group)
    {
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->IsInWorld() && member->GetMap() && member->GetMap()->IsDungeon())
                    sDynamicScaling->UpdatePlayerBuff(member);
    }
};

class DynamicScalingGlobalScript : public GlobalScript
{
public:
    DynamicScalingGlobalScript() : GlobalScript("DynamicScalingGlobalScript",
        { GLOBALHOOK_ON_ALLOWED_FOR_PLAYER_LOOT_CHECK, GLOBALHOOK_ON_ITEM_ROLL, GLOBALHOOK_ON_AFTER_REF_COUNT }) { }

    // A level range table ("Vanilla Greens 60-63 Level Range") gives way to the same kind for the scaled level
    void OnAfterRefCount(Player const* player, LootStoreItem* item, Loot& loot, bool /*canRate*/, uint16 lootMode,
        uint32& maxcount, LootStore const& store) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.lootSwap || !player || !item || !maxcount ||
            !loot.sourceWorldObjectGUID.IsCreature())
            return;

        Creature* creature = ObjectAccessor::GetCreature(*player, loot.sourceWorldObjectGUID);
        DSCreatureInfo const* info = creature ? creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY) : nullptr;
        if (!sDynamicScaling->HasScaledLoot(info))
            return;

        uint32 swap = sDynamicScaling->FindLevelLootSwap(uint32(std::abs(item->reference)), info->scaledLevel);
        LootTemplate const* replacement = swap ? LootTemplates_Reference.GetLootFor(swap) : nullptr;
        if (!replacement)
            return;

        // Done here instead of the original table, which then runs zero times
        for (uint32 loop = 0; loop < maxcount; ++loop)
            replacement->Process(loot, store, lootMode, player, 0, false);
        maxcount = 0;
    }

    // A creature scaled down drops no items needing far more than its scaled level
    bool OnItemRoll(Player const* player, LootStoreItem const* item, float& chance, Loot& loot,
        LootStore const& store) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || config.lootItemsLevelsAbove < 0 || !player || item->reference || item->needs_quest ||
            (&store != &LootTemplates_Creature && &store != &LootTemplates_Reference) ||
            !loot.sourceWorldObjectGUID.IsCreature())
            return true;

        Creature* creature = ObjectAccessor::GetCreature(*player, loot.sourceWorldObjectGUID);
        DSCreatureInfo const* info = creature ? creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY) : nullptr;
        if (!sDynamicScaling->HasScaledLoot(info) || info->scaledLevel >= info->originalLevel)
            return true;

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item->itemid);
        if (proto && int32(proto->RequiredLevel) > int32(info->scaledLevel) + config.lootItemsLevelsAbove)
            chance = 0.0f;
        return true;
    }

    // true = the player may not see this loot item
    bool OnAllowedForPlayerLootCheck(Player const* player, ObjectGuid source) override
    {
        if (!CheckingQuestLootItem)
            return false;

        CheckingQuestLootItem = false;
        return sDynamicScaling->BlocksQuestCredit(player, source);
    }
};

void AddDynamicScalingScripts()
{
    new DynamicScalingGlobalScript();
    new DynamicScalingWorldScript();
    new DynamicScalingCreatureScript();
    new DynamicScalingUnitScript();
    new DynamicScalingMapScript();
    new DynamicScalingPlayerScript();
    new DynamicScalingGroupScript();
}
