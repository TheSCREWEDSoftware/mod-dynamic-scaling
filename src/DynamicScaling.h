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

    // Players may turn scaling off for themselves (.scaling on/off)
    bool playerToggle = true;
    bool playerDefault = true;

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
    ScalingMode worldDefaultMode = ScalingMode::Off;
    uint8 worldTrailLevels = 2;
    uint8 worldLevelSpread = 1;
    LevelReference worldReference = LevelReference::Nearest;
    bool worldGroupAverage = true;
    float worldRange = 60.0f;
    uint32 worldUpdateInterval = 2000;
    uint32 worldRescaleCooldown = 10000;
    bool worldScaleNpcFlags = false;
    bool worldScaleElites = true;
    bool worldScaleRares = true;
    bool worldScaleWorldBosses = false;

    bool xpAlwaysAward = true;
    uint32 xpMinPct = 25;
    bool xpOnlyScaledZones = true;

    bool questXPEnable = true;
    uint8 questXPTrail = 2;
    uint32 questXPMinPct = 50;
    bool questXPOnlyScaledZones = true;
};

class DynamicScalingMgr
{
public:
    static DynamicScalingMgr* instance();

    void LoadConfig();
    void LoadTables();
    void ApplyEntryLevelOverrides();

    DSConfig const& Config() const { return _config; }
    uint32 GetSettingsVersion() const { return _settingsVersion; }
    void BumpSettingsVersion() { ++_settingsVersion; }

    // Settings lookups
    bool GetInstanceSettings(Map const* map, InstanceSettings& out) const;
    // Open world: area row, then zone row, then map row, then config
    bool GetZoneSettings(uint32 mapId, uint32 zoneId, uint32 areaId, ZoneSettings& out) const;
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
    uint8 ComputeInstanceLevel(Creature* creature, InstanceSettings const& settings, DSMapInfo const* mapInfo,
        uint8 originalLevel) const;
    uint8 ComputeWorldLevel(Creature* creature, ZoneSettings const& settings, uint8 referenceLevel,
        uint8 originalLevel) const;
    void ComputeMultipliers(Creature* creature, DSCreatureInfo* info) const;
    void RescaleCreature(Creature* creature, uint8 level) const;
    void ApplyArmor(Creature* creature, DSCreatureInfo const* info) const;
    void ResetCreature(Creature* creature) const;

    // Player opt in / out
    bool IsScalingFor(Player const* player) const;
    void LoadPlayerChoice(Player* player);
    void SetPlayerChoice(Player* player, bool enabled);

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
    std::unordered_map<ObjectGuid::LowType, bool> _playerChoices;
};

#define sDynamicScaling DynamicScalingMgr::instance()

#endif // MOD_DYNAMIC_SCALING_H
