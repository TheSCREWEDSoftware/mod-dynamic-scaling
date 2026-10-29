-- mod-dynamic-scaling: settings tables.
-- Lookup order: area row, then zone row (`dynamic_scaling_zone`), then map row (`dynamic_scaling_map`),
-- then the config defaults. Instances use map rows (one per difficulty, or 255 for every difficulty).

-- The Simple mode buff spell. Change it (and DynamicScaling.Instance.Simple.SpellId) if 150100 is taken.
SET @SimpleBuffSpellID := 150100;

CREATE TABLE IF NOT EXISTS `dynamic_scaling_map` (
    `map_id` INT UNSIGNED NOT NULL COMMENT 'Map.dbc ID: an instance, or a continent (0, 1, 530, 571) for open world',
    `difficulty` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '0-3, 255 = every difficulty',
    `mode` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT '0 off, 1 simple, 2 advanced, 255 config default',
    `entry_level_min` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Instances: 0 keep core requirement, 1 any level, N min level',
    `level_scaling` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'Instances: 0 off, 1 on, 255 config',
    `level_min` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT 'Lowest creature level, 0 = never below its own level',
    `level_max` TINYINT UNSIGNED NOT NULL DEFAULT 80 COMMENT 'Highest creature level, 0 = never above its own level',
    `base_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Instances: level the content was made for, 0 = LFGDungeons.dbc',
    `expected_players` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Instances: 0 = map max players',
    `simple_mult` FLOAT NOT NULL DEFAULT -1 COMMENT 'Simple: scales the player buff, -1 = 1.0',
    `health_mult` FLOAT NOT NULL DEFAULT 1,
    `damage_mult` FLOAT NOT NULL DEFAULT 1,
    `mana_mult` FLOAT NOT NULL DEFAULT 1,
    `armor_mult` FLOAT NOT NULL DEFAULT 1,
    `respawn` TINYINT UNSIGNED NOT NULL DEFAULT 255 COMMENT 'Instances: 0 off, 1 on, 255 config',
    `respawn_x` FLOAT NOT NULL DEFAULT 0 COMMENT 'All 0 = instance entrance',
    `respawn_y` FLOAT NOT NULL DEFAULT 0,
    `respawn_z` FLOAT NOT NULL DEFAULT 0,
    `respawn_o` FLOAT NOT NULL DEFAULT 0,
    `trail_levels` TINYINT NOT NULL DEFAULT -1 COMMENT 'Open world advanced: levels behind the player, -1 = config',
    `xp_min_pct` SMALLINT NOT NULL DEFAULT -1 COMMENT 'Open world advanced: XP floor percent, -1 = config',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`map_id`, `difficulty`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: per map / instance settings';

CREATE TABLE IF NOT EXISTS `dynamic_scaling_zone` (
    `zone_id` INT UNSIGNED NOT NULL COMMENT 'AreaTable.dbc ID: a zone, or an area inside a zone (area wins)',
    `mode` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '0 off, 1 simple, 2 advanced, 255 config default',
    `level_min` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = never below the creature''s own level',
    `level_max` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 = never above the creature''s own level (both 0 = unchanged)',
    `trail_levels` TINYINT NOT NULL DEFAULT -1 COMMENT 'Advanced: levels behind the player, -1 = config',
    `health_mult` FLOAT NOT NULL DEFAULT 1 COMMENT 'Advanced only',
    `damage_mult` FLOAT NOT NULL DEFAULT 1 COMMENT 'Advanced only',
    `xp_min_pct` SMALLINT NOT NULL DEFAULT -1 COMMENT 'Advanced: XP floor percent, -1 = config',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: per zone / area settings';

-- Simple mode stat buff: SPELL_AURA_MOD_TOTAL_STAT_PERCENTAGE (137) on all stats, amount set by the module.
DELETE FROM `spell_dbc` WHERE `ID` = @SimpleBuffSpellID;
INSERT INTO `spell_dbc` (`ID`, `Attributes`, `Effect_1`, `EffectAura_1`, `EffectMiscValue_1`, `Name_Lang_enUS`, `ProcChance`, `RangeIndex`, `EquippedItemClass`) VALUES
(@SimpleBuffSpellID, 448, 6, 137, -1, 'Dynamic Scaling - Stats', 101, 1, -1);
