/*
 * mod-dynamic-scaling
 *
 * One module for:
 *   - Instances (dungeons and raids), per map and difficulty:
 *       Simple   (SoloCraft style)  : players get a stat / spell power buff for every missing group member.
 *       Advanced (AutoBalance style): creatures get health / damage / mana / armor scaled by player count.
 *       Both can scale creature levels to the group, inside a level window per instance.
 *   - Open world, per zone or area (Ascension "Reborn" style):
 *       creatures trail behind the player's level inside the zone's level window,
 *       and kills / quests always award some experience.
 *   - Dungeon respawn (DungeonRespawn style): releasing inside an instance brings you back
 *     to the instance entrance alive, instead of the graveyard outside.
 *   - Instance entry level overrides (enter at any level, or at a custom minimum).
 */

#ifndef MOD_DYNAMIC_SCALING_H
#define MOD_DYNAMIC_SCALING_H

#include "DataMap.h"
#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <string>
#include <unordered_map>

// Written by mod-dynamic-scaling.cmake: whether the core has the OnPlayerGetQuestMinLevel hook
#if __has_include("DynamicScalingCoreHooks.h")
#include "DynamicScalingCoreHooks.h"
#endif
#ifndef DS_HAS_QUEST_MIN_LEVEL_HOOK
#define DS_HAS_QUEST_MIN_LEVEL_HOOK 0
#endif

class Creature;
class Map;
class Player;
class Unit;

enum class ScalingMode : uint8
{
    Off      = 0,
    Simple   = 1,
    Advanced = 2,
    Default  = 255 // use the config default
};

enum class LevelReference : uint8
{
    Highest = 0,
    Average = 1,
    Lowest  = 2,
    Nearest = 3 // world only
};

// Kinds of creature that can be kept from scaling in the open world (DynamicScaling.World.NeverScale)
enum DSCreatureCategory : uint32
{
    DS_CAT_NPC_FLAGS    = 0x001, // vendors, quest givers, trainers, flight masters...
    DS_CAT_CIVILIAN     = 0x002,
    DS_CAT_GUARD        = 0x004,
    DS_CAT_PVP          = 0x008, // friendly to Alliance or Horde players
    DS_CAT_UNATTACKABLE = 0x010,
    DS_CAT_ELITE        = 0x020,
    DS_CAT_RARE         = 0x040,
    DS_CAT_WORLD_BOSS   = 0x080,
    DS_CAT_BOSS         = 0x100  // CREATURE_FLAG_EXTRA_DUNGEON_BOSS
};

// Value in a table row that means "use the config value"
constexpr uint8 DS_USE_CONFIG = 255;

// One row of `dynamic_scaling_map` (instances by difficulty, or a whole continent)
struct InstanceScalingRow
{
    uint32 mapId = 0;
    uint8 difficulty = DS_USE_CONFIG;   // 255 = every difficulty
    ScalingMode mode = ScalingMode::Default;
    uint8 entryLevelMin = 0;            // 0 = keep the core requirement, 1 = any level
    uint8 levelScaling = DS_USE_CONFIG; // 0 off, 1 on, 255 config
    uint8 levelMin = 0;                 // 0 = never below the creature's own level
    uint8 levelMax = 0;                 // 0 = never above the creature's own level
    uint8 baseLevel = 0;                // 0 = auto (LFGDungeons.dbc)
    uint8 expectedPlayers = 0;          // 0 = map max players
    float simpleMult = -1.0f;           // < 0 = config
    float healthMult = 1.0f;
    float damageMult = 1.0f;
    float manaMult = 1.0f;
    float armorMult = 1.0f;
    uint8 respawn = DS_USE_CONFIG;      // 0 off, 1 on, 255 config
    Position respawnPos;                // all zero = auto (entrance)
    int8 trailLevels = -1;              // open world maps: < 0 = config
    int16 xpMinPct = -1;                // open world maps: < 0 = config
};

// One row of `dynamic_scaling_zone` (a zone id or an area id from AreaTable.dbc)
struct ZoneScalingRow
{
    uint32 zoneId = 0;
    ScalingMode mode = ScalingMode::Default;
    uint8 levelMin = 0;     // 0 = never below the creature's own level
    uint8 levelMax = 0;     // 0 = never above the creature's own level
    int8 trailLevels = -1;  // < 0 = config
    float healthMult = 1.0f;
    float damageMult = 1.0f;
    int16 xpMinPct = -1;    // < 0 = config
};

// Resolved settings for an instance map, after defaults are applied
struct InstanceSettings
{
    ScalingMode mode = ScalingMode::Off;
    bool levelScaling = false;
    uint8 levelMin = 0;
    uint8 levelMax = 0;
    uint8 baseLevel = 0;
    uint8 expectedPlayers = 0;
    float simpleMult = 1.0f;
    float healthMult = 1.0f;
    float damageMult = 1.0f;
    float manaMult = 1.0f;
    float armorMult = 1.0f;
    bool respawn = false;
    Position respawnPos;
};

// Resolved settings for a world zone / area
struct ZoneSettings
{
    ScalingMode mode = ScalingMode::Off;
    uint8 levelMin = 0;
    uint8 levelMax = 0;
    uint8 trailLevels = 0;
    float healthMult = 1.0f;
    float damageMult = 1.0f;
    uint32 xpMinPct = 0;
};

// Per creature state, kept in Creature::CustomData
struct DSCreatureInfo : public DataMap::Base
{
    uint8 originalLevel = 0;  // level the creature had before any scaling
    uint8 pendingLevel = 0;   // level to force on the next SelectLevel()
    uint8 scaledLevel = 0;    // 0 = not scaled
    bool goldScaled = false;  // its loot money was scaled already (this life)
    ScalingMode worldMode = ScalingMode::Off; // open world: the mode of the player it was scaled for
    float damageMult = 1.0f;
    float healthMult = 1.0f;
    float manaMult = 1.0f;
    float armorMult = 1.0f;
    uint32 mapVersion = 0;
    uint32 settingsVersion = 0;
    int32 updateTimer = 0;
    int32 cooldown = 0;
};

// Per map state, kept in Map::CustomData (instances only)
struct DSMapInfo : public DataMap::Base
{
    uint32 version = 1;
    uint32 playerCount = 0;    // every player (Advanced multipliers)
    uint32 scalingPlayers = 0; // players with scaling on (level reference)
    uint8 levelHighest = 0;
    uint8 levelLowest = 0;
    uint8 levelAverage = 0;
};

struct DSConfig
{
    bool enable = true;
    bool announce = true;

    // Collective with mod-world-randomizer: dungeons scale (Simple) inside the bracket their entrance zone landed
    // in, and the open world is left to the randomizer. Needs both modules' collective switches on.
    bool collectiveSetting = true;
    bool collective = false;

    // Players pick their own open world scaling (.scaling simple / advanced / off)
    bool playerToggle = true;
    ScalingMode playerDefault = ScalingMode::Simple;

    // Instances
    bool instanceEnable = true;
    ScalingMode instanceDefaultMode = ScalingMode::Simple;
    bool instanceLevelScaling = true;
    LevelReference instanceReference = LevelReference::Highest;
    uint8 instanceDefaultLevelMin = 1;
    uint8 instanceDefaultLevelMax = 80;
    bool instanceIncludeRaids = true;
    bool instanceIncludeHeroics = true;

    uint32 simpleSpellId = 150100;
    float simpleStatsMult = 100.0f;
    float simpleSpellPowerMult = 1.0f;
    bool simpleXPPenalty = false;

    float advancedHealthFloor = 0.3f;
    float advancedDamageFloor = 0.3f;
    float advancedManaFloor = 0.5f;
    float advancedHealthMult = 1.0f;
    float advancedDamageMult = 1.0f;

    bool respawnEnable = true;
    bool respawnRaids = true;
    float respawnHealthPct = 50.0f;

    // World
    bool worldEnable = true;
    bool worldDefaultScaled = false;       // places with no zone, area or map row
    uint8 worldSimpleTrail = 3;            // Simple: creatures sit this many levels below you, anywhere
    uint8 worldTrailLevels = 2;            // Advanced: default trail inside the zone's level window
    uint8 worldLevelSpread = 1;
    LevelReference worldReference = LevelReference::Nearest;
    bool worldGroupAverage = true;
    float worldRange = 0.0f;               // 0 = the map's visibility distance
    bool worldSilentLevelChange = true;    // re-send the creature instead of a visible level change
    bool worldScaleOnEngage = true;        // rescale to the player a creature engages
    bool questMinLevel = true;             // lower quest level requirements where the player is scaled
    uint32 worldUpdateInterval = 2000;
    uint32 worldRescaleCooldown = 10000;
    bool worldUseDBOverride = false;       // categories from `dynamic_scaling_category` instead of NeverScale
    uint32 worldNeverScale = DS_CAT_NPC_FLAGS | DS_CAT_CIVILIAN | DS_CAT_GUARD | DS_CAT_PVP | DS_CAT_UNATTACKABLE |
        DS_CAT_WORLD_BOSS | DS_CAT_BOSS;

    // Players with scaling off: kills this many levels below them give no quest progress or quest items (0 = off)
    uint8 offQuestCreditLevels = 5;

    bool xpAlwaysAward = true;
    uint32 xpMinPct = 25;
    bool xpOnlyScaledZones = true;

    bool questXPEnable = true;
    uint8 questXPTrail = 2;
    uint32 questXPMinPct = 50;
    bool questXPOnlyScaledZones = true;

    // Rewards follow the level you are scaled to
    bool questXPCap = true;                // quests above your level give the XP of a quest at your level
    bool questMoneyScale = true;           // ...and money scaled the same way
    bool lootMoneyScale = true;            // a creature's gold follows its scaled level
    bool lootSwap = true;                  // level range loot tables follow the creature's scaled level
    uint8 lootSwapExpansion = 0;           // 0 the level's expansion first, 1 the table's own first, 2 only its own
    uint8 lootSwapTbcFrom = 61;            // levels that count as TBC / Wrath for LootSwap.Expansion 0
    uint8 lootSwapWrathFrom = 71;
    uint8 rewardMarginDown = 3;            // scaled this close to its own level (or a quest this close above
    uint8 rewardMarginUp = 3;              // you), rewards stay normal
    int32 lootItemsLevelsAbove = 5;        // scaled down creatures drop no items needing this many levels more
                                           // than their scaled level (-1 = off)
};

class DynamicScalingMgr
{
public:
    static DynamicScalingMgr* instance();

    void LoadConfig();
    void LoadTables();
    void ApplyEntryLevelOverrides();
    // No OnPlayerGetQuestMinLevel hook: lower quest levels in memory, for everyone, the way PlayerToggle.Default
    // would (Simple: 1, Advanced: the zone's level_min). Undone on reload when it no longer applies.
    void ApplyQuestMinLevelFallback();
    // Money for a level (quest_money_reward, middle difficulty): the curve gold and quest money are scaled along
    static float MoneyForLevel(uint8 level);
    // The "<Expansion> <Kind> <min>-<max> Level Range" reference tables (1EKLLHH): the table of the same kind for
    // a level, or 0 when the given one already covers it
    uint32 FindLevelLootSwap(uint32 referenceId, uint8 level) const;
    std::size_t LevelLootCount() const { return _levelLoot.size(); }
    std::string LevelLootName(uint32 referenceId) const;
    // Scaled further from its own level than the reward margins: its loot and gold follow the scaled level
    bool HasScaledLoot(DSCreatureInfo const* info) const;
    void LoadCollective();
    std::size_t CollectiveCount() const { return _collectiveBrackets.size(); }

    DSConfig const& Config() const { return _config; }
    uint32 GetSettingsVersion() const { return _settingsVersion; }
    void BumpSettingsVersion() { ++_settingsVersion; }

    // Settings lookups
    bool GetInstanceSettings(Map const* map, InstanceSettings& out) const;
    // Open world: area row, then zone row, then map row, then config. playerMode (Simple or Advanced) picks how
    // the row is used: Simple trails behind the player anywhere, Advanced keeps the row's level window and fields.
    bool GetZoneSettings(uint32 mapId, uint32 zoneId, uint32 areaId, ZoneSettings& out,
        ScalingMode playerMode) const;
    float GetWorldRange(Map const* map) const;
    InstanceScalingRow const* FindInstanceRow(uint32 mapId, uint8 difficulty) const;
    ZoneScalingRow const* FindZoneRow(uint32 id) const;
    std::size_t InstanceRowCount() const { return _instanceRows.size(); }
    std::size_t ZoneRowCount() const { return _zoneRows.size(); }

    // Instance map tracking
    DSMapInfo* GetMapInfo(Map* map) const;
    void RefreshMapInfo(Map* map) const;
    uint8 GetReferenceLevel(DSMapInfo const* info) const;

    // Creature scaling
    bool IsCreatureEligible(Creature* creature, bool world) const;
    // Empty when the creature can scale in the open world, else why not
    std::string NeverScaleReason(Creature* creature) const;
    uint32 GetNeverScaleMask() const { return _config.worldUseDBOverride ? _dbNeverScale : _config.worldNeverScale; }
    static uint32 ParseCategories(std::string const& text);
    static std::string CategoryNames(uint32 mask);
    // What the creature would be for this player: its own level, and in Simple / Advanced (0 = not scaled there)
    bool PreviewWorldLevels(Creature* creature, Player* player, uint8& own, uint8& simple, uint8& advanced) const;
    uint8 ComputeInstanceLevel(Creature* creature, InstanceSettings const& settings, DSMapInfo const* mapInfo,
        uint8 originalLevel) const;
    uint8 ComputeWorldLevel(Creature* creature, ZoneSettings const& settings, uint8 referenceLevel,
        uint8 originalLevel) const;
    void ComputeMultipliers(Creature* creature, DSCreatureInfo* info) const;
    void RescaleCreature(Creature* creature, uint8 level, bool allowSilent = true) const;
    void ApplyArmor(Creature* creature, DSCreatureInfo const* info) const;
    void ResetCreature(Creature* creature) const;
    bool IsTargetedByViewer(Creature* creature) const;

    // Player choice: Off, Simple or Advanced
    ScalingMode GetPlayerMode(Player const* player) const;
    bool IsScalingFor(Player const* player) const { return GetPlayerMode(player) != ScalingMode::Off; }
    void LoadPlayerChoice(Player* player);
    void SetPlayerChoice(Player* player, ScalingMode mode);
    static char const* ModeName(ScalingMode mode);
    // Scaling off: a kill DynamicScaling.World.Off.NoQuestCreditLevels or more levels below the player gives no
    // quest progress or quest items (open world, scaled places only)
    bool BlocksQuestCredit(Player const* player, ObjectGuid source) const;

    // Player buff (instance Simple mode)
    void UpdatePlayerBuff(Player* player);
    void RemovePlayerBuff(Player* player);
    float GetPlayerXPModifier(Player* player) const;

    // Dungeon respawn
    void RecordEntryPosition(Player* player);
    void ForgetPlayer(Player* player);
    bool ResolveRespawnPosition(Player* player, InstanceSettings const& settings, Position& out) const;

    static uint8 ClampLevel(int32 level);

private:
    DSConfig _config;
    uint32 _settingsVersion = 1;

    std::unordered_map<uint64, InstanceScalingRow> _instanceRows; // key: mapId << 8 | difficulty
    std::unordered_map<uint32, ZoneScalingRow> _zoneRows;
    std::unordered_map<uint32, bool> _creatureRules;              // creature entry -> always / never scales
    std::unordered_map<uint32, uint32> _questMinLevelBackup;      // quest -> its own minimum level (fallback)

    struct LevelLootTable
    {
        std::string kind;   // greens, blues, potions... (lowercase)
        uint8 expansion = 0; // 0 Vanilla, 1 TBC, 2 Wrath
        uint8 levelMin = 0;
        uint8 levelMax = 0;
    };
    std::unordered_map<uint32, LevelLootTable> _levelLoot;
    std::unordered_map<std::string, std::vector<uint32>> _levelLootByKind;
    void LoadLevelLoot();
    uint32 _dbNeverScale = 0;                                     // from `dynamic_scaling_category`
    bool CheckWorldEligible(Creature* creature, uint8 depth, std::string* reason) const;
    std::unordered_map<uint64, uint8> _originalEntryLevels;       // access requirement levelMin backups

    struct PlayerBuffState
    {
        uint32 spellPowerBonus = 0;
        float xpModifier = 1.0f;
    };
    std::unordered_map<ObjectGuid::LowType, PlayerBuffState> _playerBuffs;

    struct EntryPosition
    {
        uint32 mapId = 0;
        Position pos;
    };
    std::unordered_map<ObjectGuid::LowType, EntryPosition> _entryPositions;
    std::unordered_map<ObjectGuid::LowType, ScalingMode> _playerChoices;
    std::unordered_map<uint32, std::pair<uint8, uint8>> _collectiveBrackets; // dungeon map -> randomizer bracket
    std::unordered_map<ObjectGuid::LowType, int32> _lastAnnounced; // stat percent last told to the player
};

#define sDynamicScaling DynamicScalingMgr::instance()

#endif // MOD_DYNAMIC_SCALING_H
