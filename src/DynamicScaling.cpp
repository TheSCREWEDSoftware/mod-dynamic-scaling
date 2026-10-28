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
#include "LootMgr.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "SpellAuraEffects.h"
#include "SpellMgr.h"
#include "Tokenize.h"
#include "UpdateData.h"
#include "WorldPacket.h"

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
    c.playerDefault = ToMode(sConfigMgr->GetOption<uint32>("DynamicScaling.PlayerToggle.Default", 1),
        ScalingMode::Simple);
    if (c.playerDefault == ScalingMode::Default)
        c.playerDefault = ScalingMode::Simple;

    // The other module's settings live in the same config manager; quiet lookups, since it may not be installed
    c.collectiveSetting = sConfigMgr->GetOption<bool>("DynamicScaling.Collective.WorldRandomizer", true);
    c.collective = c.collectiveSetting &&
        sConfigMgr->GetOption<bool>("Randomizer.Enable", false, false) &&
        sConfigMgr->GetOption<bool>("Randomizer.World.Enable", false, false) &&
        sConfigMgr->GetOption<bool>("Randomizer.Collective.DynamicScaling", false, false);

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
    c.worldDefaultScaled = sConfigMgr->GetOption<uint32>("DynamicScaling.World.DefaultMode", 0) != 0;
    c.worldSimpleTrail = sConfigMgr->GetOption<uint8>("DynamicScaling.World.Simple.TrailLevels", 3);
    c.worldTrailLevels = sConfigMgr->GetOption<uint8>("DynamicScaling.World.TrailLevels", 2);
    c.worldLevelSpread = sConfigMgr->GetOption<uint8>("DynamicScaling.World.LevelSpread", 1);
    c.worldReference = ToReference(sConfigMgr->GetOption<uint32>("DynamicScaling.World.Reference", 3),
        LevelReference::Nearest);
    c.worldGroupAverage = sConfigMgr->GetOption<bool>("DynamicScaling.World.GroupAverage", true);
    c.worldRange = sConfigMgr->GetOption<float>("DynamicScaling.World.Range", 0.0f);
    c.worldSilentLevelChange = sConfigMgr->GetOption<bool>("DynamicScaling.World.SilentLevelChange", true);
    c.worldScaleOnEngage = sConfigMgr->GetOption<bool>("DynamicScaling.World.ScaleOnEngage", true);
    c.questMinLevel = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestMinLevel.Enable", true);
    c.worldUpdateInterval = sConfigMgr->GetOption<uint32>("DynamicScaling.World.UpdateInterval", 2000);
    c.worldRescaleCooldown = sConfigMgr->GetOption<uint32>("DynamicScaling.World.RescaleCooldown", 10000);
    c.worldUseDBOverride = sConfigMgr->GetOption<bool>("DynamicScaling.World.UseDBOverride", false);
    c.worldNeverScale = ParseCategories(sConfigMgr->GetOption<std::string>("DynamicScaling.World.NeverScale",
        "NpcFlags Civilian Guard PvP Unattackable WorldBoss Boss"));
    c.offQuestCreditLevels = sConfigMgr->GetOption<uint8>("DynamicScaling.World.Off.NoQuestCreditLevels", 5);

    c.xpAlwaysAward = sConfigMgr->GetOption<bool>("DynamicScaling.World.XP.AlwaysAward", true);
    c.xpMinPct = sConfigMgr->GetOption<uint32>("DynamicScaling.World.XP.MinPct", 25);
    c.xpOnlyScaledZones = sConfigMgr->GetOption<bool>("DynamicScaling.World.XP.OnlyScaledZones", true);

    c.questXPEnable = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestXP.Enable", true);
    c.questXPTrail = sConfigMgr->GetOption<uint8>("DynamicScaling.World.QuestXP.TrailLevels", 2);
    c.questXPMinPct = sConfigMgr->GetOption<uint32>("DynamicScaling.World.QuestXP.MinPct", 50);
    c.questXPOnlyScaledZones = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestXP.OnlyScaledZones", true);
    c.questXPCap = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestXP.Cap", true);
    c.questMoneyScale = sConfigMgr->GetOption<bool>("DynamicScaling.World.QuestMoney.Scale", true);
    c.lootMoneyScale = sConfigMgr->GetOption<bool>("DynamicScaling.World.LootMoney.Scale", true);
    c.lootItemsLevelsAbove = sConfigMgr->GetOption<int32>("DynamicScaling.World.LootItems.MaxLevelsAbove", 5);
    c.lootSwap = sConfigMgr->GetOption<bool>("DynamicScaling.World.LootSwap.Enable", true);
    c.lootSwapExpansion =
        std::min<uint8>(2, sConfigMgr->GetOption<uint8>("DynamicScaling.World.LootSwap.Expansion", 0));
    c.lootSwapTbcFrom = sConfigMgr->GetOption<uint8>("DynamicScaling.World.LootSwap.TBCFromLevel", 61);
    c.lootSwapWrathFrom = sConfigMgr->GetOption<uint8>("DynamicScaling.World.LootSwap.WrathFromLevel", 71);
    c.rewardMarginDown = sConfigMgr->GetOption<uint8>("DynamicScaling.World.Rewards.MarginDown", 3);
    c.rewardMarginUp = sConfigMgr->GetOption<uint8>("DynamicScaling.World.Rewards.MarginUp", 3);

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

    _creatureRules.clear();
    if (QueryResult result = WorldDatabase.Query("SELECT `entry`, `scale` FROM `dynamic_scaling_creature`"))
    {
        do
        {
            Field* fields = result->Fetch();
            _creatureRules[fields[0].Get<uint32>()] = fields[1].Get<uint8>() != 0;
        } while (result->NextRow());
    }

    LoadLevelLoot();

    _dbNeverScale = 0;
    if (QueryResult result = WorldDatabase.Query("SELECT `category`, `scale` FROM `dynamic_scaling_category`"))
    {
        do
        {
            Field* fields = result->Fetch();
            uint32 category = ParseCategories(fields[0].Get<std::string>());
            if (!category)
                LOG_ERROR("module", "mod-dynamic-scaling: unknown category '{}' in `dynamic_scaling_category`.",
                    fields[0].Get<std::string>());
            else if (!fields[1].Get<uint8>())
                _dbNeverScale |= category;
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-dynamic-scaling: loaded {} map rows, {} zone/area rows and {} creature rules; never "
        "scale: {}.", _instanceRows.size(), _zoneRows.size(), _creatureRules.size(),
        CategoryNames(GetNeverScaleMask()));

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

namespace
{
    // Quest::MinLevel is protected; a pointer to it taken through a derived class is the legal way in
    struct QuestMinLevelAccess : Quest
    {
        static constexpr uint32 Quest::* Field = &QuestMinLevelAccess::MinLevel;
    };
}

void DynamicScalingMgr::ApplyQuestMinLevelFallback()
{
    // Put every quest back first, so a reload or a config change starts from the real values
    for (auto const& [questId, minLevel] : _questMinLevelBackup)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(questId))
            const_cast<Quest*>(quest)->*QuestMinLevelAccess::Field = minLevel;
    _questMinLevelBackup.clear();

    ScalingMode mode = _config.playerDefault;
    if (DS_HAS_QUEST_MIN_LEVEL_HOOK || !_config.enable || !_config.worldEnable || !_config.questMinLevel ||
        mode == ScalingMode::Off)
        return;

    uint32 changed = 0;
    for (auto const& [questId, quest] : sObjectMgr->GetQuestTemplates())
    {
        if (quest->GetMinLevel() <= 1 || quest->GetZoneOrSort() <= 0)
            continue;

        AreaTableEntry const* zone = sAreaTableStore.LookupEntry(uint32(quest->GetZoneOrSort()));
        MapEntry const* map = zone ? sMapStore.LookupEntry(zone->mapid) : nullptr;
        ZoneSettings settings;
        if (!map || map->Instanceable() || !GetZoneSettings(zone->mapid, zone->ID, 0, settings, mode))
            continue;

        uint32 minLevel = mode == ScalingMode::Simple ? 1 :
            settings.levelMin ? std::min<uint32>(quest->GetMinLevel(), settings.levelMin) : quest->GetMinLevel();
        if (minLevel == quest->GetMinLevel())
            continue;

        _questMinLevelBackup[questId] = quest->GetMinLevel();
        quest->*QuestMinLevelAccess::Field = minLevel;
        ++changed;
    }

    LOG_INFO("module", "mod-dynamic-scaling: no OnPlayerGetQuestMinLevel hook in the core, lowered the level "
        "requirement of {} quests in scaled zones for everyone ({}).", changed, ModeName(mode));
}

void DynamicScalingMgr::LoadLevelLoot()
{
    _levelLoot.clear();
    _levelLootByKind.clear();

    // The kind comes from the comment of any row pointing at the table ("Chatter - Vanilla Greens 8-12 Level
    // Range"), the level range from its id: 1, expansion, kind digit, two digits min, two digits max
    QueryResult result = WorldDatabase.Query("SELECT `Reference`, `Comment` FROM `creature_loot_template` WHERE "
        "`Reference` BETWEEN 1000000 AND 1299999 AND `Comment` LIKE '%Level Range%' UNION SELECT `Reference`, "
        "`Comment` FROM `reference_loot_template` WHERE `Reference` BETWEEN 1000000 AND 1299999 AND `Comment` LIKE "
        "'%Level Range%'");
    if (!result)
        return;

    do
    {
        Field* fields = result->Fetch();
        uint32 id = fields[0].Get<uint32>();
        if (_levelLoot.count(id) || !LootTemplates_Reference.HaveLootFor(id))
            continue;

        std::vector<std::string_view> words = Acore::Tokenize(fields[1].Get<std::string_view>(), ' ', false);
        std::string kind;
        bool inKind = false;
        for (std::string_view word : words)
        {
            if (word == "Vanilla" || word == "TBC" || word == "WotLK" || word == "WoLK")
            {
                inKind = true;
                kind.clear();
                continue;
            }
            if (!inKind)
                continue;
            if (!word.empty() && word[0] >= '0' && word[0] <= '9')
                break;
            kind += (kind.empty() ? "" : " ") + std::string(word);
        }
        if (kind.empty())
            continue;
        std::transform(kind.begin(), kind.end(), kind.begin(), ::tolower);

        LevelLootTable table;
        table.kind = kind;
        table.expansion = uint8((id / 100000) % 10);
        table.levelMin = uint8((id / 100) % 100);
        table.levelMax = uint8(id % 100);
        if (!table.levelMin || table.levelMax < table.levelMin)
            continue;

        _levelLoot[id] = table;
        _levelLootByKind[kind].push_back(id);
    } while (result->NextRow());
}

uint32 DynamicScalingMgr::FindLevelLootSwap(uint32 referenceId, uint8 level) const
{
    auto itr = _levelLoot.find(referenceId);
    if (itr == _levelLoot.end() || (level >= itr->second.levelMin && level <= itr->second.levelMax))
        return 0;

    // Same kind: a table covering the level (else the closest), of the preferred expansion if there is one,
    // centered nearest to the level
    uint8 wantedExpansion = itr->second.expansion;
    if (_config.lootSwapExpansion == 0)
        wantedExpansion = level >= _config.lootSwapWrathFrom ? 2 : level >= _config.lootSwapTbcFrom ? 1 : 0;

    uint32 best = 0;
    int32 bestScore = 0;
    for (uint32 id : _levelLootByKind.at(itr->second.kind))
    {
        LevelLootTable const& table = _levelLoot.at(id);
        if (_config.lootSwapExpansion == 2 && table.expansion != itr->second.expansion)
            continue;

        int32 outside = level < table.levelMin ? table.levelMin - level :
            level > table.levelMax ? level - table.levelMax : 0;
        int32 center = std::abs(int32(table.levelMin + table.levelMax) - 2 * int32(level));
        int32 score = outside * 1000 + (table.expansion == wantedExpansion ? 0 : 100) + center;
        if (!best || score < bestScore)
        {
            best = id;
            bestScore = score;
        }
    }

    return best == referenceId ? 0 : best;
}

bool DynamicScalingMgr::HasScaledLoot(DSCreatureInfo const* info) const
{
    if (!info || !info->scaledLevel || !info->originalLevel)
        return false;

    int32 diff = int32(info->scaledLevel) - int32(info->originalLevel);
    return diff < -int32(_config.rewardMarginDown) || diff > int32(_config.rewardMarginUp);
}

std::string DynamicScalingMgr::LevelLootName(uint32 referenceId) const
{
    static char const* const EXPANSIONS[] = { "Vanilla", "TBC", "WotLK" };
    auto itr = _levelLoot.find(referenceId);
    if (itr == _levelLoot.end())
        return "not a level range table";

    return Acore::StringFormat("{} {} {}-{}", EXPANSIONS[std::min<uint8>(itr->second.expansion, 2)],
        itr->second.kind, itr->second.levelMin, itr->second.levelMax);
}

float DynamicScalingMgr::MoneyForLevel(uint8 level)
{
    return float(std::max<uint32>(1, sObjectMgr->GetQuestMoneyReward(std::clamp<uint8>(level, 1, 80), 5)));
}

void DynamicScalingMgr::LoadCollective()
{
    _collectiveBrackets.clear();
    if (!_config.collective)
        return;

    // Written by mod-world-randomizer at startup (before this runs, at OnStartup)
    if (QueryResult result = WorldDatabase.Query("SELECT `map_id`, `level_min`, `level_max` "
        "FROM `world_randomizer_dungeon_bracket`"))
    {
        do
        {
            Field* fields = result->Fetch();
            _collectiveBrackets[fields[0].Get<uint32>()] = { fields[1].Get<uint8>(), fields[2].Get<uint8>() };
        } while (result->NextRow());
    }

    LOG_INFO("module", "mod-dynamic-scaling: collective with mod-world-randomizer, {} dungeons scale inside their "
        "bracket; open world scaling is left to the randomizer.", _collectiveBrackets.size());
    BumpSettingsVersion();
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

    // Collective: Simple mode, scaling inside the bracket the randomizer gave this dungeon's entrance zone
    if (_config.collective)
    {
        auto itr = _collectiveBrackets.find(map->GetId());
        if (itr != _collectiveBrackets.end())
        {
            out.mode = ScalingMode::Simple;
            out.levelScaling = true;
            out.levelMin = itr->second.first;
            out.levelMax = itr->second.second;
        }
    }

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

bool DynamicScalingMgr::GetZoneSettings(uint32 mapId, uint32 zoneId, uint32 areaId, ZoneSettings& out,
    ScalingMode playerMode) const
{
    out = ZoneSettings();
    if (playerMode != ScalingMode::Simple && playerMode != ScalingMode::Advanced)
        return false;

    // Collective with the randomizer: it owns open world levels
    if (!_config.enable || !_config.worldEnable || _config.collective)
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

    // A row with mode 0, or with both sides of the window 0 (cities, starting areas), is never scaled; any other
    // row is scaled the way the player chose. Places with no row follow DynamicScaling.World.DefaultMode.
    if (row ? row->mode == ScalingMode::Off || (!row->levelMin && !row->levelMax) : !_config.worldDefaultScaled)
        return false;

    out.mode = playerMode;
    out.xpMinPct = _config.xpMinPct;

    // Simple: trail behind the player anywhere, the zone's level range doesn't matter
    if (playerMode == ScalingMode::Simple)
    {
        out.levelMin = 1;
        out.levelMax = MAX_LEVEL;
        out.trailLevels = _config.worldSimpleTrail;
        return true;
    }

    out.levelMin = row ? row->levelMin : 0;
    out.levelMax = row ? row->levelMax : 0;
    out.trailLevels = _config.worldTrailLevels;

    if (row)
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

float DynamicScalingMgr::GetWorldRange(Map const* map) const
{
    if (_config.worldRange > 0.0f || !map)
        return _config.worldRange > 0.0f ? _config.worldRange : 100.0f;

    return map->GetVisibilityRange();
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

namespace
{
    struct CategoryName
    {
        uint32 category;
        char const* name;
    };

    constexpr CategoryName CATEGORY_NAMES[] =
    {
        { DS_CAT_NPC_FLAGS, "NpcFlags" }, { DS_CAT_CIVILIAN, "Civilian" }, { DS_CAT_GUARD, "Guard" },
        { DS_CAT_PVP, "PvP" }, { DS_CAT_UNATTACKABLE, "Unattackable" }, { DS_CAT_ELITE, "Elite" },
        { DS_CAT_RARE, "Rare" }, { DS_CAT_WORLD_BOSS, "WorldBoss" }, { DS_CAT_BOSS, "Boss" }
    };

    uint32 CreatureCategories(Creature const* creature)
    {
        uint32 categories = 0;
        if (creature->GetNpcFlags() != UNIT_NPC_FLAG_NONE)
            categories |= DS_CAT_NPC_FLAGS;
        if (creature->IsCivilian())
            categories |= DS_CAT_CIVILIAN;
        if (creature->IsGuard())
            categories |= DS_CAT_GUARD;
        if (creature->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE) || creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE))
            categories |= DS_CAT_UNATTACKABLE;
        if (creature->IsDungeonBoss())
            categories |= DS_CAT_BOSS;

        if (FactionTemplateEntry const* faction = creature->GetFactionTemplateEntry())
        {
            if (faction->ourMask & (FACTION_MASK_ALLIANCE | FACTION_MASK_HORDE))
                categories |= DS_CAT_PVP;
            else
                for (uint32 playerFaction : { 1u, 2u }) // Human, Orc
                    if (FactionTemplateEntry const* side = sFactionTemplateStore.LookupEntry(playerFaction))
                        if (faction->IsFriendlyTo(*side))
                            categories |= DS_CAT_PVP;
        }

        switch (creature->GetCreatureTemplate()->rank)
        {
            case CREATURE_ELITE_ELITE:
                categories |= DS_CAT_ELITE;
                break;
            case CREATURE_ELITE_RAREELITE:
                categories |= DS_CAT_ELITE | DS_CAT_RARE;
                break;
            case CREATURE_ELITE_RARE:
                categories |= DS_CAT_RARE;
                break;
            case CREATURE_ELITE_WORLDBOSS:
                categories |= DS_CAT_WORLD_BOSS;
                break;
            default:
                break;
        }

        return categories;
    }
}

uint32 DynamicScalingMgr::ParseCategories(std::string const& text)
{
    uint32 mask = 0;
    std::string word;
    auto flush = [&]()
    {
        for (CategoryName const& entry : CATEGORY_NAMES)
            if (!word.empty() && StringEqualI(word, entry.name))
                mask |= entry.category;
        word.clear();
    };

    for (char c : text)
    {
        if (c == ' ' || c == ',' || c == ';' || c == '|')
            flush();
        else
            word += c;
    }
    flush();
    return mask;
}

std::string DynamicScalingMgr::CategoryNames(uint32 mask)
{
    std::string names;
    for (CategoryName const& entry : CATEGORY_NAMES)
        if (mask & entry.category)
            names += (names.empty() ? "" : ", ") + std::string(entry.name);
    return names.empty() ? "none" : names;
}

bool DynamicScalingMgr::IsCreatureEligible(Creature* creature, bool world) const
{
    if (!creature || creature->IsPet() || creature->IsTotem() || creature->IsCritter() || creature->IsTrigger())
        return false;

    if (creature->GetCharmerOrOwnerGUID().IsPlayer() || !creature->GetCreatureTemplate())
        return false;

    return !world || CheckWorldEligible(creature, 0, nullptr);
}

std::string DynamicScalingMgr::NeverScaleReason(Creature* creature) const
{
    if (!creature)
        return "no creature";

    if (creature->IsPet() || creature->IsTotem() || creature->IsCritter() || creature->IsTrigger() ||
        creature->GetCharmerOrOwnerGUID().IsPlayer())
        return "pet, totem, critter or trigger";

    std::string reason;
    CheckWorldEligible(creature, 0, &reason);
    return reason;
}

bool DynamicScalingMgr::CheckWorldEligible(Creature* creature, uint8 depth, std::string* reason) const
{
    // `dynamic_scaling_creature` beats everything below
    auto rule = _creatureRules.find(creature->GetEntry());
    if (rule != _creatureRules.end())
    {
        if (!rule->second && reason)
            *reason = "creature rule (dynamic_scaling_creature)";
        return rule->second;
    }

    // Pets and summons of a creature (a guard's gryphon, a flight master's defenders) follow their owner
    Unit* owner = creature->GetOwner();
    if (!owner && creature->IsSummon())
        owner = creature->ToTempSummon()->GetSummonerUnit();
    if (owner && owner != creature && owner->IsCreature() && depth < 2)
    {
        bool eligible = CheckWorldEligible(owner->ToCreature(), depth + 1, reason);
        if (!eligible && reason)
            *reason = "its owner " + owner->GetName() + " never scales: " + *reason;
        return eligible;
    }

    uint32 blocked = CreatureCategories(creature) & GetNeverScaleMask();
    if (blocked && reason)
        *reason = CategoryNames(blocked) + (_config.worldUseDBOverride ? " (dynamic_scaling_category)" :
            " (DynamicScaling.World.NeverScale)");
    return !blocked;
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

bool DynamicScalingMgr::PreviewWorldLevels(Creature* creature, Player* player, uint8& own, uint8& simple,
    uint8& advanced) const
{
    DSCreatureInfo const* info = creature->CustomData.Get<DSCreatureInfo>(CREATURE_INFO_KEY);
    own = info && info->originalLevel ? info->originalLevel : creature->GetLevel();
    simple = 0;
    advanced = 0;

    if (!creature->GetMap() || creature->GetMap()->Instanceable() || !IsCreatureEligible(creature, true))
        return false;

    ZoneSettings settings;
    if (GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings,
        ScalingMode::Simple))
        simple = ComputeWorldLevel(creature, settings, player->GetLevel(), own);
    if (GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings,
        ScalingMode::Advanced))
        advanced = ComputeWorldLevel(creature, settings, player->GetLevel(), own);

    return simple || advanced;
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
    if (info->worldMode != ScalingMode::Advanced ||
        !GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings,
        ScalingMode::Advanced))
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

void DynamicScalingMgr::RescaleCreature(Creature* creature, uint8 level, bool allowSilent) const
{
    DSCreatureInfo* info = creature->CustomData.GetDefault<DSCreatureInfo>(CREATURE_INFO_KEY);

    // SelectLevel() rebuilds health, mana, damage and attack power for the new level;
    // the SelectLevel hooks pick up pendingLevel and apply the multipliers.
    info->pendingLevel = level;

    // Someone with it selected would lose the target when it is sent again: change it the normal way then
    bool silent = allowSilent && _config.worldSilentLevelChange && level != creature->GetLevel() &&
        !IsTargetedByViewer(creature);
    if (!silent)
    {
        creature->SelectLevel();
        ApplyArmor(creature, info);
        creature->UpdateAllStats();
        return;
    }

    // The client plays a level up effect when it sees a unit's level go up. Change the level without sending the
    // field, then destroy the creature on each client and create it again with the new level.
    creature->SetLevel(level, false);
    creature->SelectLevel(false);
    ApplyArmor(creature, info);
    creature->UpdateAllStats();

    std::vector<Player*> viewers;
    creature->DoForAllVisiblePlayers([&](Player* player)
    {
        if (player && player->HaveAtClient(creature))
            viewers.push_back(player);
    });

    for (Player* player : viewers)
    {
        // SMSG_DESTROY_OBJECT removes it at once; an out of range block in the same packet as the create left the
        // client reusing the old object, which still showed the level going up
        creature->DestroyForPlayer(player);

        UpdateData data;
        creature->BuildCreateUpdateBlockForPlayer(&data, player);
        WorldPacket packet;
        data.BuildPacket(packet);
        player->SendDirectMessage(&packet);
        player->GetInitialVisiblePackets(creature);
    }
}

bool DynamicScalingMgr::IsTargetedByViewer(Creature* creature) const
{
    bool targeted = false;
    creature->DoForAllVisiblePlayers([&](Player* player)
    {
        if (player && player->GetTarget() == creature->GetGUID())
            targeted = true;
    });
    return targeted;
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

ScalingMode DynamicScalingMgr::GetPlayerMode(Player const* player) const
{
    if (!player || !_config.playerToggle)
        return _config.playerDefault == ScalingMode::Off ? ScalingMode::Simple : _config.playerDefault;

    auto itr = _playerChoices.find(player->GetGUID().GetCounter());
    return itr != _playerChoices.end() ? itr->second : _config.playerDefault;
}

bool DynamicScalingMgr::BlocksQuestCredit(Player const* player, ObjectGuid source) const
{
    if (!player || !_config.enable || !_config.worldEnable || !_config.playerToggle || !_config.offQuestCreditLevels)
        return false;

    if (GetPlayerMode(player) != ScalingMode::Off || !source.IsCreatureOrVehicle())
        return false;

    Creature const* creature = ObjectAccessor::GetCreature(*player, source);
    if (!creature || !creature->GetMap() || creature->GetMap()->Instanceable())
        return false;

    ZoneSettings settings;
    if (!GetZoneSettings(creature->GetMapId(), creature->GetZoneId(), creature->GetAreaId(), settings,
        ScalingMode::Advanced))
        return false;

    return int32(creature->GetLevel()) + int32(_config.offQuestCreditLevels) <= int32(player->GetLevel());
}

char const* DynamicScalingMgr::ModeName(ScalingMode mode)
{
    switch (mode)
    {
        case ScalingMode::Simple:
            return "Simple";
        case ScalingMode::Advanced:
            return "Advanced";
        default:
            return "Off";
    }
}

void DynamicScalingMgr::LoadPlayerChoice(Player* player)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    _playerChoices.erase(guid);

    if (QueryResult result = CharacterDatabase.Query("SELECT `enabled` FROM `dynamic_scaling_player` WHERE `guid` = {}",
        guid))
        _playerChoices[guid] = ToMode((*result)[0].Get<uint8>(), _config.playerDefault);
}

void DynamicScalingMgr::SetPlayerChoice(Player* player, ScalingMode mode)
{
    ObjectGuid::LowType guid = player->GetGUID().GetCounter();
    _playerChoices[guid] = mode;
    CharacterDatabase.Execute("REPLACE INTO `dynamic_scaling_player` (`guid`, `enabled`) VALUES ({}, {})", guid,
        uint32(mode));

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
