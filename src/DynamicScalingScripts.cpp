/*
 * mod-dynamic-scaling: script hooks.
 */

#include "DynamicScaling.h"

#include "Chat.h"
#include "Creature.h"
#include "DBCStores.h"
#include "Formulas.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Map.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "SpellInfo.h"
#include "World.h"

#include <algorithm>
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

    // Players waiting for a dungeon respawn after releasing their spirit
    std::unordered_set<ObjectGuid::LowType> PendingRespawns;
}

class DynamicScalingWorldScript : public WorldScript
{
public:
    DynamicScalingWorldScript() : WorldScript("DynamicScalingWorldScript",
        { WORLDHOOK_ON_AFTER_CONFIG_LOAD, WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED }) { }

    void OnAfterConfigLoad(bool reload) override
    {
        sDynamicScaling->LoadConfig();
        if (reload)
            sDynamicScaling->ApplyEntryLevelOverrides();
    }

    void OnBeforeWorldInitialized() override
    {
        sDynamicScaling->LoadTables();
        sDynamicScaling->ApplyEntryLevelOverrides();
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

        Map* map = creature->GetMap();
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

        ZoneSettings settings;
        if (!sDynamicScaling->GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(),
            settings) ||
            !sDynamicScaling->IsCreatureEligible(creature, true))
        {
            if (info->scaledLevel)
                sDynamicScaling->ResetCreature(creature);
            return;
        }

        if (info->cooldown > 0 && !settingsChanged)
            return;

        if (!info->originalLevel)
            info->originalLevel = creature->GetLevel();

        bool onlyOptedOut = false;
        uint8 reference = GetWorldReferenceLevel(creature, config, onlyOptedOut);
        if (!reference)
        {
            // Only players who turned scaling off are watching: show them the real level
            if (onlyOptedOut && info->scaledLevel)
            {
                sDynamicScaling->ResetCreature(creature);
                info->cooldown = int32(config.worldRescaleCooldown);
            }
            return;
        }

        uint8 target = sDynamicScaling->ComputeWorldLevel(creature, settings, reference, info->originalLevel);
        if (target == creature->GetLevel() && !settingsChanged)
            return;

        sDynamicScaling->RescaleCreature(creature, target);
        info->cooldown = int32(config.worldRescaleCooldown);
    }

    uint8 GetWorldReferenceLevel(Creature* creature, DSConfig const& config, bool& onlyOptedOut)
    {
        onlyOptedOut = false;
        std::list<Player*> players;
        Acore::AnyPlayerInObjectRangeCheck check(creature, config.worldRange, true, true);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(creature, players, check);
        Cell::VisitObjects(creature, searcher, config.worldRange);

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
public:
    DynamicScalingUnitScript() : UnitScript("DynamicScalingUnitScript", true, {
        UNITHOOK_MODIFY_MELEE_DAMAGE,
        UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
        UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK
    }) { }

    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        if (DSCreatureInfo const* info = FindInfo(attacker))
            if (info->damageMult != 1.0f)
                damage = uint32(float(damage) * info->damageMult);
    }

    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* /*spellInfo*/) override
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

    void OnPlayerEnterAll(Map* map, Player* /*player*/) override
    {
        if (map && map->IsDungeon())
            sDynamicScaling->RefreshMapInfo(map);
    }

    void OnPlayerLeaveAll(Map* map, Player* /*player*/) override
    {
        if (map && map->IsDungeon())
            sDynamicScaling->RefreshMapInfo(map);
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
        PLAYERHOOK_ON_QUEST_COMPUTE_EXP
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        DSConfig const& config = sDynamicScaling->Config();
        sDynamicScaling->LoadPlayerChoice(player);

        if (config.enable && config.announce)
        {
            ChatHandler handler(player->GetSession());
            handler.SendSysMessage("This server is running the |cff4CFF00Dynamic Scaling|r module.");
            if (config.playerToggle)
                handler.PSendSysMessage("Scaling is {} for you. Type |cffFFFF00.scaling on|r or |cffFFFF00.scaling off|r "
                    "to change it.", sDynamicScaling->IsScalingFor(player) ? "|cff4CFF00on|r" : "|cffFF0000off|r");
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
        PendingRespawns.erase(player->GetGUID().GetCounter());
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
            PendingRespawns.insert(player->GetGUID().GetCounter());
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 /*mapId*/, float /*x*/, float /*y*/, float /*z*/,
        float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        auto itr = PendingRespawns.find(player->GetGUID().GetCounter());
        if (itr == PendingRespawns.end())
            return true;

        // Erase first: the TeleportTo() below comes back through this hook
        PendingRespawns.erase(itr);

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
        DSConfig const& config = sDynamicScaling->Config();
        if (!config.enable || !config.worldEnable || !config.questXPEnable || !config.questXPMinPct)
            return;

        // 0 means "no XP on purpose" (already rewarded, or a no-XP quest)
        if (!xpValue || quest->GetQuestLevel() <= 0 || !sDynamicScaling->IsScalingFor(player))
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
            if (!sDynamicScaling->GetZoneSettings(mapId, zoneId, 0, settings))
                return;
        }

        uint8 playerLevel = player->GetLevel();
        int32 effectiveLevel = std::max<int32>(quest->GetQuestLevel(), int32(playerLevel) - int32(config.questXPTrail));
        QuestXPEntry const* entry = sQuestXPStore.LookupEntry(effectiveLevel);
        if (!entry || quest->GetXPId() >= 10)
            return;

        // Same math as Quest::XPValue(), with the quest level trailing the player
        int32 diffFactor = std::clamp<int32>(2 * (effectiveLevel - int32(playerLevel)) + 20, 1, 10);
        float floorXP = float(diffFactor * entry->Exp[quest->GetXPId()] / 10);
        floorXP *= player->GetQuestRate(quest->IsDFQuest());
        floorXP *= player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_QUEST_PCT);
        floorXP *= float(config.questXPMinPct) / 100.0f;

        xpValue = std::max(xpValue, uint32(floorXP));
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

        ZoneSettings settings;
        bool scaled = sDynamicScaling->GetZoneSettings(killed->GetMapId(), killed->GetZoneId(), killed->GetAreaId(),
            settings);
        if (!scaled && config.xpOnlyScaledZones)
            return;

        uint32 minPct = scaled ? settings.xpMinPct : config.xpMinPct;
        uint8 trail = scaled ? settings.trailLevels : config.worldTrailLevels;
        if (!minPct)
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

            uint8 level = player->GetLevel();
            uint8 trailingLevel = DynamicScalingMgr::ClampLevel(int32(level) - int32(trail));
            uint32 xp = Acore::XP::BaseGain(level, trailingLevel,
                GetContentLevelsForMapAndZone(killed->GetMapId(), killed->GetZoneId()));
            xp = uint32(float(xp) * sWorld->getRate(RATE_XP_KILL) * float(minPct) / 100.0f);
            xp = uint32(float(xp) * player->GetTotalAuraMultiplier(SPELL_AURA_MOD_XP_PCT));
            if (receivers.size() > 1)
                xp /= uint32(receivers.size());

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

void AddDynamicScalingScripts()
{
    new DynamicScalingWorldScript();
    new DynamicScalingCreatureScript();
    new DynamicScalingUnitScript();
    new DynamicScalingMapScript();
    new DynamicScalingPlayerScript();
    new DynamicScalingGroupScript();
}
