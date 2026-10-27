/*
 * mod-dynamic-scaling: settings, tables and the shared scaling math.
 */

#include "DynamicScaling.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"

#include <algorithm>

namespace
{
    constexpr char const* CREATURE_INFO_KEY = "DynamicScalingCreatureInfo";
    constexpr char const* MAP_INFO_KEY = "DynamicScalingMapInfo";

    uint64 MakeInstanceKey(uint32 mapId, uint8 difficulty)
    {
        return (uint64(mapId) << 8) | difficulty;
    }

    ScalingMode ToMode(uint32 value, ScalingMode fallback)
    {
        switch (value)
        {
            case 0:
                return ScalingMode::Off;
            case 1:
                return ScalingMode::Simple;
            case 2:
                return ScalingMode::Advanced;
            case 255:
                return ScalingMode::Default;
            default:
                return fallback;
        }
    }

    LevelReference ToReference(uint32 value, LevelReference fallback)
    {
        if (value > uint32(LevelReference::Nearest))
            return fallback;

        return LevelReference(value);
    }

    float Lerp(float floor, float ratio)
    {
        floor = std::clamp(floor, 0.0f, 1.0f);
        ratio = std::clamp(ratio, 0.0f, 1.0f);
        return floor + (1.0f - floor) * ratio;
    }
}

DynamicScalingMgr* DynamicScalingMgr::instance()
{
    static DynamicScalingMgr instance;
    return &instance;
}

uint8 DynamicScalingMgr::ClampLevel(int32 level)
{
    return uint8(std::clamp<int32>(level, 1, MAX_LEVEL));
}

void DynamicScalingMgr::LoadConfig()
{
    DSConfig c;

    c.enable = sConfigMgr->GetOption<bool>("DynamicScaling.Enable", true);
    c.announce = sConfigMgr->GetOption<bool>("DynamicScaling.Announce", true);
    c.playerToggle = sConfigMgr->GetOption<bool>("DynamicScaling.PlayerToggle.Enable", true);
    c.playerDefault = sConfigMgr->GetOption<bool>("DynamicScaling.PlayerToggle.Default", true);

    c.instanceEnable = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.Enable", true);
    c.instanceDefaultMode = ToMode(sConfigMgr->GetOption<uint32>("DynamicScaling.Instance.DefaultMode", 1),
        ScalingMode::Simple);
    if (c.instanceDefaultMode == ScalingMode::Default)
        c.instanceDefaultMode = ScalingMode::Simple;
    c.instanceIncludeRaids = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.IncludeRaids", true);
    c.instanceIncludeHeroics = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.IncludeHeroics", true);
    c.instanceLevelScaling = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.LevelScaling.Enable", true);
    c.instanceReference = ToReference(
        sConfigMgr->GetOption<uint32>("DynamicScaling.Instance.LevelScaling.Reference", 0), LevelReference::Highest);
    if (c.instanceReference == LevelReference::Nearest)
        c.instanceReference = LevelReference::Highest;
    c.instanceDefaultLevelMin =
        sConfigMgr->GetOption<uint8>("DynamicScaling.Instance.LevelScaling.DefaultLevelMin", 1);
    c.instanceDefaultLevelMax =
        sConfigMgr->GetOption<uint8>("DynamicScaling.Instance.LevelScaling.DefaultLevelMax", 80);

    c.simpleSpellId = sConfigMgr->GetOption<uint32>("DynamicScaling.Instance.Simple.SpellId", 150100);
    c.simpleStatsMult = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Simple.StatsMult", 100.0f);
    c.simpleSpellPowerMult = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Simple.SpellPowerMult", 1.0f);
    c.simpleXPPenalty = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.Simple.XPPenalty", false);

    c.advancedHealthFloor = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Advanced.HealthFloor", 0.3f);
    c.advancedDamageFloor = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Advanced.DamageFloor", 0.3f);
    c.advancedManaFloor = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Advanced.ManaFloor", 0.5f);
    c.advancedHealthMult = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Advanced.HealthMult", 1.0f);
    c.advancedDamageMult = sConfigMgr->GetOption<float>("DynamicScaling.Instance.Advanced.DamageMult", 1.0f);

    c.respawnEnable = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.Respawn.Enable", true);
    c.respawnRaids = sConfigMgr->GetOption<bool>("DynamicScaling.Instance.Respawn.Raids", true);
    c.respawnHealthPct = std::clamp(sConfigMgr->GetOption<float>("DynamicScaling.Instance.Respawn.HealthPct", 50.0f),
        1.0f, 100.0f);

    c.worldEnable = sConfigMgr->GetOption<bool>("DynamicScaling.World.Enable", true);
    c.worldDefaultMode = ToMode(sConfigMgr->GetOption<uint32>("DynamicScaling.World.DefaultMode", 0), ScalingMode::Off);
    if (c.worldDefaultMode == ScalingMode::Default)
        c.worldDefaultMode = ScalingMode::Off;
    c.worldTrailLevels = sConfigMgr->GetOption<uint8>("DynamicScaling.World.TrailLevels", 2);
    c.worldLevelSpread = sConfigMgr->GetOption<uint8>("DynamicScaling.World.LevelSpread", 1);
    c.worldReference = ToReference(sConfigMgr->GetOption<uint32>("DynamicScaling.World.Reference", 3),
        LevelReference::Nearest);
    c.worldGroupAverage = sConfigMgr->GetOption<bool>("DynamicScaling.World.GroupAverage", true);
    c.worldRange = sConfigMgr->GetOption<float>("DynamicScaling.World.Range", 60.0f);
    c.worldUpdateInterval = sConfigMgr->GetOption<uint32>("DynamicScaling.World.UpdateInterval", 2000);
    c.worldRescaleCooldown = sConfigMgr->GetOption<uint32>("DynamicScaling.World.RescaleCooldown", 10000);
    c.worldScaleNpcFlags = sConfigMgr->GetOption<bool>("DynamicScaling.World.ScaleNpcFlags", false);
    c.worldScaleElites = sConfigMgr->GetOption<bool>("DynamicScaling.World.ScaleElites", true);
    c.worldScaleRares = sConfigMgr->GetOption<bool>("DynamicScaling.World.ScaleRares", true);
    c.worldScaleWorldBosses = sConfigMgr->GetOption<bool>("DynamicScaling.World.ScaleWorldBosses", false);

    c.xpAlwaysAward = sConfigMgr->GetOption<bool>("DynamicScaling.World.XP.AlwaysAward", true);
    c.xpMinPct = sConfigMgr->GetOption<uint32>("DynamicScaling.World.XP.MinPct", 25);
    c.xpOnlyScaledZones = sConfigMgr->GetOption<bool>("DynamicScaling.World.XP.OnlyScaledZones", true);

    c.questXPEnable = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestXP.Enable", true);
    c.questXPTrail = sConfigMgr->GetOption<uint8>("DynamicScaling.World.QuestXP.TrailLevels", 2);
    c.questXPMinPct = sConfigMgr->GetOption<uint32>("DynamicScaling.World.QuestXP.MinPct", 50);
    c.questXPOnlyScaledZones = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestXP.OnlyScaledZones", true);

    _config = c;
    BumpSettingsVersion();
}

void DynamicScalingMgr::LoadTables()
{
    _instanceRows.clear();
    _zoneRows.clear();

    //                                                   0         1             2       3
    if (QueryResult result = WorldDatabase.Query("SELECT `map_id`, `difficulty`, `mode`, `entry_level_min`, "
        //   4
        "`level_scaling`, "
        //   5            6            7             8                   9              10
        "`level_min`, `level_max`, `base_level`, `expected_players`, `simple_mult`, `health_mult`, "
        //   11             12           13           14         15           16           17           18
        "`damage_mult`, `mana_mult`, `armor_mult`, `respawn`, `respawn_x`, `respawn_y`, `respawn_z`, `respawn_o`, "
        //   19              20
        "`trail_levels`, `xp_min_pct` FROM `dynamic_scaling_map`"))
    {
        do
        {
            Field* fields = result->Fetch();

            InstanceScalingRow row;
            row.mapId = fields[0].Get<uint32>();
            row.difficulty = fields[1].Get<uint8>();
            row.mode = ToMode(fields[2].Get<uint8>(), ScalingMode::Default);
            row.entryLevelMin = fields[3].Get<uint8>();
            row.levelScaling = fields[4].Get<uint8>();
            row.levelMin = fields[5].Get<uint8>();
            row.levelMax = fields[6].Get<uint8>();
            row.baseLevel = fields[7].Get<uint8>();
            row.expectedPlayers = fields[8].Get<uint8>();
            row.simpleMult = fields[9].Get<float>();
            row.healthMult = fields[10].Get<float>();
            row.damageMult = fields[11].Get<float>();
            row.manaMult = fields[12].Get<float>();
            row.armorMult = fields[13].Get<float>();
            row.respawn = fields[14].Get<uint8>();
            row.respawnPos.Relocate(fields[15].Get<float>(), fields[16].Get<float>(), fields[17].Get<float>(),
                fields[18].Get<float>());
            row.trailLevels = fields[19].Get<int8>();
            row.xpMinPct = fields[20].Get<int16>();

            if (!sMapStore.LookupEntry(row.mapId))
            {
                LOG_ERROR("module", "mod-dynamic-scaling: `dynamic_scaling_map` has unknown map_id {}, skipped.",
                    row.mapId);
                continue;
            }

            _instanceRows[MakeInstanceKey(row.mapId, row.difficulty)] = row;
        } while (result->NextRow());
    }

    //                                                   0          1       2            3            4
    if (QueryResult result = WorldDatabase.Query("SELECT `zone_id`, `mode`, `level_min`, `level_max`, `trail_levels`, "
        //   5              6              7
        "`health_mult`, `damage_mult`, `xp_min_pct` FROM `dynamic_scaling_zone`"))
    {
        do
        {
            Field* fields = result->Fetch();

            ZoneScalingRow row;
            row.zoneId = fields[0].Get<uint32>();
            row.mode = ToMode(fields[1].Get<uint8>(), ScalingMode::Default);
            row.levelMin = fields[2].Get<uint8>();
            row.levelMax = fields[3].Get<uint8>();
            row.trailLevels = fields[4].Get<int8>();
            row.healthMult = fields[5].Get<float>();
            row.damageMult = fields[6].Get<float>();
            row.xpMinPct = fields[7].Get<int16>();

            _zoneRows[row.zoneId] = row;
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-dynamic-scaling: loaded {} map rows and {} zone/area rows.", _instanceRows.size(),
        _zoneRows.size());

    if (!sSpellMgr->GetSpellInfo(_config.simpleSpellId))
        LOG_ERROR("module", "mod-dynamic-scaling: spell {} (DynamicScaling.Instance.Simple.SpellId) is missing from "
            "`spell_dbc`. Simple mode will only give spell power, not stats.", _config.simpleSpellId);

    BumpSettingsVersion();
}

void DynamicScalingMgr::ApplyEntryLevelOverrides()
{
    // Restore what the core loaded, so removed rows stop overriding
    for (auto const& [key, levelMin] : _originalEntryLevels)
    {
        uint32 mapId = uint32(key >> 8);
        Difficulty difficulty = Difficulty(key & 0xFF);
        if (DungeonProgressionRequirements const* ar = sObjectMgr->GetAccessRequirement(mapId, difficulty))
            const_cast<DungeonProgressionRequirements*>(ar)->levelMin = levelMin;
    }

    if (!_config.enable || !_config.instanceEnable)
        return;

    uint32 count = 0;
    for (auto const& [key, row] : _instanceRows)
    {
        if (!row.entryLevelMin)
            continue;

        for (uint8 diff = 0; diff < MAX_DIFFICULTY; ++diff)
        {
            if (row.difficulty != DS_USE_CONFIG && row.difficulty != diff)
                continue;

            DungeonProgressionRequirements const* ar = sObjectMgr->GetAccessRequirement(row.mapId, Difficulty(diff));
            if (!ar)
                continue;

            uint64 arKey = MakeInstanceKey(row.mapId, diff);
            if (_originalEntryLevels.find(arKey) == _originalEntryLevels.end())
                _originalEntryLevels[arKey] = ar->levelMin;

            const_cast<DungeonProgressionRequirements*>(ar)->levelMin = row.entryLevelMin;
            ++count;
        }
    }

    if (count)
        LOG_INFO("module", "mod-dynamic-scaling: overrode {} instance entry level requirements.", count);
}

InstanceScalingRow const* DynamicScalingMgr::FindInstanceRow(uint32 mapId, uint8 difficulty) const
{
    auto itr = _instanceRows.find(MakeInstanceKey(mapId, difficulty));
    if (itr != _instanceRows.end())
        return &itr->second;

    itr = _instanceRows.find(MakeInstanceKey(mapId, DS_USE_CONFIG));
    if (itr != _instanceRows.end())
        return &itr->second;

    return nullptr;
}

ZoneScalingRow const* DynamicScalingMgr::FindZoneRow(uint32 id) const
{
    auto itr = _zoneRows.find(id);
    return itr != _zoneRows.end() ? &itr->second : nullptr;
}

bool DynamicScalingMgr::GetInstanceSettings(Map const* map, InstanceSettings& out) const
{
    out = InstanceSettings();

    if (!_config.enable || !_config.instanceEnable || !map || !map->IsDungeon())
        return false;

    if (map->IsRaid() && !_config.instanceIncludeRaids)
        return false;

    if (map->IsHeroic() && !_config.instanceIncludeHeroics)
        return false;

    uint8 difficulty = uint8(map->GetDifficulty());
    InstanceScalingRow const* row = FindInstanceRow(map->GetId(), difficulty);

    out.mode = row && row->mode != ScalingMode::Default ? row->mode : _config.instanceDefaultMode;
    out.levelScaling = row && row->levelScaling != DS_USE_CONFIG ? row->levelScaling != 0 :
        _config.instanceLevelScaling;
    out.levelMin = row ? row->levelMin : _config.instanceDefaultLevelMin;
    out.levelMax = row ? row->levelMax : _config.instanceDefaultLevelMax;
    out.expectedPlayers = row ? row->expectedPlayers : 0;
    out.simpleMult = row && row->simpleMult >= 0.0f ? row->simpleMult : 1.0f;
    out.healthMult = row ? row->healthMult : 1.0f;
    out.damageMult = row ? row->damageMult : 1.0f;
    out.manaMult = row ? row->manaMult : 1.0f;
    out.armorMult = row ? row->armorMult : 1.0f;
    out.respawn = row && row->respawn != DS_USE_CONFIG ? row->respawn != 0 :
        (_config.respawnEnable && (!map->IsRaid() || _config.respawnRaids));
    if (row)
        out.respawnPos = row->respawnPos;

    if (!out.expectedPlayers)
        if (InstanceMap const* instance = const_cast<Map*>(map)->ToInstanceMap())
            out.expectedPlayers = uint8(instance->GetMaxPlayers());

    if (!out.expectedPlayers)
        out.expectedPlayers = 5;

    out.baseLevel = row ? row->baseLevel : 0;
    if (!out.baseLevel)
    {
        // Auto: the recommended level of the matching LFG dungeon entry
        uint32 bestLevel = 0;
        for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
        {
            LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i);
            if (!entry || entry->MapID != map->GetId())
                continue;

            uint32 level = entry->TargetLevel ? entry->TargetLevel : (entry->MinLevel + entry->MaxLevel) / 2;
            if (!level)
                continue;

            if (entry->Difficulty == difficulty)
            {
                bestLevel = level;
                break;
            }

            if (!bestLevel)
                bestLevel = level;
        }

        out.baseLevel = uint8(bestLevel);
    }

    return out.mode != ScalingMode::Off || out.levelScaling || out.respawn;
}

bool DynamicScalingMgr::GetZoneSettings(uint32 mapId, uint32 zoneId, uint32 areaId, ZoneSettings& out) const
{
    out = ZoneSettings();

    if (!_config.enable || !_config.worldEnable)
        return false;

    // The most specific row wins: area, then zone, then the whole map
    ZoneScalingRow const* row = areaId && areaId != zoneId ? FindZoneRow(areaId) : nullptr;
    if (!row)
        row = FindZoneRow(zoneId);

    ZoneScalingRow mapRow;
    if (!row)
    {
        if (InstanceScalingRow const* map = FindInstanceRow(mapId, DS_USE_CONFIG))
        {
            mapRow.zoneId = 0;
            mapRow.mode = map->mode;
            mapRow.levelMin = map->levelMin;
            mapRow.levelMax = map->levelMax;
            mapRow.trailLevels = map->trailLevels;
            mapRow.healthMult = map->healthMult;
            mapRow.damageMult = map->damageMult;
            mapRow.xpMinPct = map->xpMinPct;
            row = &mapRow;
        }
    }

    out.mode = row && row->mode != ScalingMode::Default ? row->mode : _config.worldDefaultMode;
    if (out.mode == ScalingMode::Off)
        return false;

    out.levelMin = row ? row->levelMin : 0;
    out.levelMax = row ? row->levelMax : 0;

    // Both sides 0 means "keep the creature's own level" (starting zones, cities)
    if (!out.levelMin && !out.levelMax && row)
    {
        out.mode = ScalingMode::Off;
        return false;
    }

    out.trailLevels = _config.worldTrailLevels;
    out.xpMinPct = _config.xpMinPct;

    if (out.mode == ScalingMode::Advanced && row)
    {
        if (row->trailLevels >= 0)
            out.trailLevels = uint8(row->trailLevels);
        if (row->xpMinPct >= 0)
            out.xpMinPct = uint32(row->xpMinPct);
        out.healthMult = row->healthMult;
        out.damageMult = row->damageMult;
    }

    return true;
}

DSMapInfo* DynamicScalingMgr::GetMapInfo(Map* map) const
{
    return map ? map->CustomData.GetDefault<DSMapInfo>(MAP_INFO_KEY) : nullptr;
}

void DynamicScalingMgr::RefreshMapInfo(Map* map) const
{
    DSMapInfo* info = GetMapInfo(map);
    if (!info)
        return;

    uint32 total = 0;
    uint32 count = 0;
    uint32 sum = 0;
    uint8 highest = 0;
    uint8 lowest = 0;

    for (auto const& ref : map->GetPlayers())
    {
        Player* player = ref.GetSource();
        if (!player || player->IsGameMaster())
            continue;

        ++total;

        // Players who turned scaling off do not pull the creature levels
        if (!IsScalingFor(player))
            continue;

        uint8 level = player->GetLevel();
        ++count;
        sum += level;
        highest = std::max(highest, level);
        lowest = lowest ? std::min(lowest, level) : level;
    }

    uint8 average = count ? uint8((sum + count / 2) / count) : 0;

    if (info->playerCount != total || info->scalingPlayers != count || info->levelHighest != highest ||
        info->levelLowest != lowest || info->levelAverage != average)
    {
        info->playerCount = total;
        info->scalingPlayers = count;
        info->levelHighest = highest;
        info->levelLowest = lowest;
        info->levelAverage = average;
        ++info->version;
    }
}

uint8 DynamicScalingMgr::GetReferenceLevel(DSMapInfo const* info) const
{
    if (!info || !info->scalingPlayers)
        return 0;

    switch (_config.instanceReference)
    {
        case LevelReference::Average:
            return info->levelAverage;
        case LevelReference::Lowest:
            return info->levelLowest;
        default:
            return info->levelHighest;
    }
}

bool DynamicScalingMgr::IsCreatureEligible(Creature* creature, bool world) const
{
    if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsCritter() || creature->IsTrigger())
        return false;

    if (creature->GetCharmerOrOwnerGUID().IsPlayer())
        return false;

    CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
    if (!cInfo)
        return false;

    if (!world)
        return true;

    if (creature->IsCivilian())
        return false;

    if (!_config.worldScaleNpcFlags && creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
        return false;

    if (creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
        return false;

    switch (cInfo->rank)
    {
        case CREATURE_ELITE_ELITE:
            return _config.worldScaleElites;
        case CREATURE_ELITE_RAREELITE:
            return _config.worldScaleElites && _config.worldScaleRares;
        case CREATURE_ELITE_RARE:
            return _config.worldScaleRares;
        case CREATURE_ELITE_WORLDBOSS:
            return _config.worldScaleWorldBosses;
        default:
            return true;
    }
}

uint8 DynamicScalingMgr::ComputeInstanceLevel(Creature* /*creature*/, InstanceSettings const& settings,
    DSMapInfo const* mapInfo, uint8 originalLevel) const
{
    if (!settings.levelScaling || !settings.baseLevel)
        return originalLevel;

    uint8 reference = GetReferenceLevel(mapInfo);
    if (!reference)
        return originalLevel;

    // Shift every creature by the same amount, so bosses stay above their trash
    int32 delta = int32(reference) - int32(settings.baseLevel);
    int32 target = int32(originalLevel) + delta;
    int32 aboveBase = std::max<int32>(0, int32(originalLevel) - int32(settings.baseLevel));

    // A side set to 0 is bound by the creature's own level
    int32 lower = settings.levelMin ? int32(settings.levelMin) + aboveBase : int32(originalLevel);
    int32 upper = settings.levelMax ? int32(settings.levelMax) + aboveBase : int32(originalLevel);
    if (lower > upper)
        lower = upper;

    return ClampLevel(std::clamp(target, lower, upper));
}

uint8 DynamicScalingMgr::ComputeWorldLevel(Creature* creature, ZoneSettings const& settings, uint8 referenceLevel,
    uint8 originalLevel) const
{
    if (!referenceLevel)
        return originalLevel;

    // Keep a little of the template's level range, so a pack is not all the same level
    CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
    int32 templateMin = std::min(cInfo->minlevel, cInfo->maxlevel);
    int32 spread = std::clamp<int32>(int32(originalLevel) - templateMin, 0, _config.worldLevelSpread);

    int32 target = int32(referenceLevel) - int32(settings.trailLevels) + spread;
    int32 lower = settings.levelMin ? int32(settings.levelMin) : int32(originalLevel);
    int32 upper = settings.levelMax ? int32(settings.levelMax) : int32(originalLevel);
    if (lower > upper)
        lower = upper;

    return ClampLevel(std::clamp(target, lower, upper));
}

void DynamicScalingMgr::ComputeMultipliers(Creature* creature, DSCreatureInfo* info) const
{
    info->healthMult = 1.0f;
    info->damageMult = 1.0f;
    info->manaMult = 1.0f;
    info->armorMult = 1.0f;

    Map* map = creature->GetMap();
    if (!map)
        return;

    if (map->IsDungeon())
    {
        InstanceSettings settings;
        if (!GetInstanceSettings(map, settings) || settings.mode != ScalingMode::Advanced)
            return;

        DSMapInfo const* mapInfo = GetMapInfo(map);
        uint32 players = std::max<uint32>(1, mapInfo ? mapInfo->playerCount : 1);
        float ratio = float(players) / float(std::max<uint8>(1, settings.expectedPlayers));

        info->healthMult = Lerp(_config.advancedHealthFloor, ratio) * settings.healthMult * _config.advancedHealthMult;
        info->damageMult = Lerp(_config.advancedDamageFloor, ratio) * settings.damageMult * _config.advancedDamageMult;
        info->manaMult = Lerp(_config.advancedManaFloor, ratio) * settings.manaMult;
        info->armorMult = settings.armorMult;
        return;
    }

    if (map->Instanceable() || !info->scaledLevel)
        return;

    ZoneSettings settings;
    if (!GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings) ||
        settings.mode != ScalingMode::Advanced)
        return;

    info->healthMult = settings.healthMult;
    info->damageMult = settings.damageMult;
}

void DynamicScalingMgr::ApplyArmor(Creature* creature, DSCreatureInfo const* info) const
{
    CreatureTemplate const* cInfo = creature->GetCreatureTemplate();
    CreatureBaseStats const* stats = sObjectMgr->GetCreatureBaseStats(creature->GetLevel(), cInfo->unit_class);
    if (!stats)
        return;

    float armor = stats->GenerateArmor(cInfo) * (info ? info->armorMult : 1.0f);
    creature->SetStatFlatModifier(UNIT_MOD_ARMOR, BASE_VALUE, armor);
}

void DynamicScalingMgr::RescaleCreature(Creature* creature, uint8 level) const
{
    DSCreatureInfo* info = creature->CustomData.GetDefault<DSCreatureInfo>(CREATURE_INFO_KEY);

    // SelectLevel() rebuilds health, mana, damage and attack power for the new level;
    // the SelectLevel hooks pick up pendingLevel and apply the multipliers.
    info->pendingLevel = level;
    creature->SelectLevel();

    ApplyArmor(creature, info);
    creature->UpdateAllStats();
}

void DynamicScalingMgr::ResetCreature(Creature* creature) const
{
    DSCreatureInfo* info = creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY);
    if (!info || !info->scaledLevel || !info->originalLevel)
        return;

    uint8 original = info->originalLevel;
    RescaleCreature(creature, original);
    info->scaledLevel = 0;
}

void DynamicScalingMgr::UpdatePlayerBuff(Player* player)
{
    RemovePlayerBuff(player);

    Map* map = player->GetMap();
    InstanceSettings settings;
    if (!IsScalingFor(player) || !GetInstanceSettings(map, settings) || settings.mode != ScalingMode::Simple)
    {
        _lastAnnounced.erase(player->GetGUID().GetCounter());
        return;
    }

    uint32 groupSize = 1;
    if (Group* group = player->GetGroup())
    {
        groupSize = 0;
        for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (member->GetMap() == map && !member->IsGameMaster())
                    ++groupSize;

        groupSize = std::max<uint32>(1, groupSize);
    }

    uint32 expected = std::max<uint32>(1, settings.expectedPlayers);
    PlayerBuffState state;

    if (_config.simpleXPPenalty)
        state.xpModifier = std::min(1.0f, float(groupSize) / float(expected));

    if (groupSize >= expected)
    {
        _playerBuffs[player->GetGUID().GetCounter()] = state;
        return;
    }

    // Every missing player adds one "player worth" of stats (StatsMult = 100)
    float missing = float(expected) / float(groupSize) - 1.0f;
    float statPct = missing * _config.simpleStatsMult * settings.simpleMult;

    if (sSpellMgr->GetSpellInfo(_config.simpleSpellId))
    {
        if (!player->HasAura(_config.simpleSpellId))
            player->AddAura(_config.simpleSpellId, player);

        if (AuraEffect* effect = player->GetAuraEffect(_config.simpleSpellId, EFFECT_0))
            effect->ChangeAmount(int32(statPct));
    }

    if (player->getPowerType() == POWER_MANA || player->getClass() == CLASS_DRUID)
    {
        // Only the flat gear bonus: the %-stat buff above would feed back into stat-based spell power
        uint32 base = std::max({ player->GetBaseSpellPowerBonus(), player->GetBaseSpellDamageBonus(),
            player->GetBaseSpellHealingBonus() });
        state.spellPowerBonus = uint32(float(base) * _config.simpleSpellPowerMult * missing * settings.simpleMult);
        if (state.spellPowerBonus)
            player->ApplySpellPowerBonus(int32(state.spellPowerBonus), true);
    }

    _playerBuffs[player->GetGUID().GetCounter()] = state;

    if (player->IsAlive() && !player->IsInCombat())
    {
        player->SetFullHealth();
        if (player->getPowerType() == POWER_MANA)
            player->SetPower(POWER_MANA, player->GetMaxPower(POWER_MANA));
    }

    int32& lastAnnounced = _lastAnnounced[player->GetGUID().GetCounter()];
    if (_config.announce && lastAnnounced != int32(statPct))
        ChatHandler(player->GetSession()).PSendSysMessage("|cff4CFF00[Dynamic Scaling]|r {}: group {}/{}, "
            "stats +{:.0f}%, spell power +{}.", map->GetMapName(), groupSize, expected, statPct, state.spellPowerBonus);
    lastAnnounced = int32(statPct);
}

void DynamicScalingMgr::RemovePlayerBuff(Player* player)
{
    auto itr = _playerBuffs.find(player->GetGUID().GetCounter());
    if (itr != _playerBuffs.end())
    {
        if (itr->second.spellPowerBonus)
            player->ApplySpellPowerBonus(int32(itr->second.spellPowerBonus), false);

        _playerBuffs.erase(itr);
    }

    if (_config.simpleSpellId && player->HasAura(_config.simpleSpellId))
        player->RemoveAurasDueToSpell(_config.simpleSpellId);
}

float DynamicScalingMgr::GetPlayerXPModifier(Player* player) const
{
    auto itr = _playerBuffs.find(player->GetGUID().GetCounter());
    return itr != _playerBuffs.end() ? itr->second.xpModifier : 1.0f;
}

void DynamicScalingMgr::RecordEntryPosition(Player* player)
{
    Map* map = player->GetMap();
    if (!map || !map->IsDungeon())
        return;

    EntryPosition& entry = _entryPositions[player->GetGUID().GetCounter()];
    entry.mapId = map->GetId();
    entry.pos.Relocate(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ(),
        player->GetOrientation());
}

void DynamicScalingMgr::ForgetPlayer(Player* player)
{
    _entryPositions.erase(player->GetGUID().GetCounter());
    _playerBuffs.erase(player->GetGUID().GetCounter());
    _playerChoices.erase(player->GetGUID().GetCounter());
    _lastAnnounced.erase(player->GetGUID().GetCounter());
}

bool DynamicScalingMgr::IsScalingFor(Player const* player) const
{
    if (!player || !_config.playerToggle)
        return true;

    auto itr = _playerChoices.find(player->GetGUID().GetCounter());
    return itr != _playerChoices.end() ? itr->second : _config.playerDefault;
}

void DynamicScalingMgr::LoadPlayerChoice(Player* player)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    _playerChoices.erase(guid);

    if (QueryResult result = CharacterDatabase.Query("SELECT `enabled` FROM `dynamic_scaling_player` WHERE `guid` = {}",
        guid))
        _playerChoices[guid] = (*result)[0].Get<bool>();
}

void DynamicScalingMgr::SetPlayerChoice(Player* player, bool enabled)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    _playerChoices[guid] = enabled;
    CharacterDatabase.Execute("REPLACE INTO `dynamic_scaling_player` (`guid`, `enabled`) VALUES ({}, {})", guid,
        enabled ? 1 : 0);

    if (Map* map = player->GetMap())
        if (map->IsDungeon())
            RefreshMapInfo(map);

    UpdatePlayerBuff(player);
}

bool DynamicScalingMgr::ResolveRespawnPosition(Player* player, InstanceSettings const& settings, Position& out) const
{
    // 1. Manual coordinates from the table
    if (settings.respawnPos.GetPositionX() != 0.0f || settings.respawnPos.GetPositionY() != 0.0f ||
        settings.respawnPos.GetPositionZ() != 0.0f)
    {
        out = settings.respawnPos;
        return true;
    }

    // 2. The instance entrance (where the entrance portal drops you)
    if (AreaTriggerTeleport const* at = sObjectMgr->GetMapEntranceTrigger(player->GetMapId()))
    {
        out.Relocate(at->target_X, at->target_Y, at->target_Z, at->target_Orientation);
        return true;
    }

    // 3. Where this player arrived in this instance
    auto itr = _entryPositions.find(player->GetGUID().GetCounter());
    if (itr != _entryPositions.end() && itr->second.mapId == player->GetMapId())
    {
        out = itr->second.pos;
        return true;
    }

    return false;
}
