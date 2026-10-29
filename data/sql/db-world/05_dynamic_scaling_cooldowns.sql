-- mod-dynamic-scaling: cooldown resets in instances, battlegrounds, arenas and duels (the rules are used with
-- DynamicScaling.CooldownReset.UseDBOverride = 1; the creature, encounter and spell lists are always used).
-- The cooldown reset mechanic is by sogladev.

-- Which instances and creatures reset cooldowns (1) or not (0)
SET @ResetDungeon := 0;
SET @ResetDungeonHeroic := 0;
SET @ResetRaid10 := 1;
SET @ResetRaid25 := 1;           -- 20 and 40 player raids too
SET @ResetRaid10Heroic := 1;
SET @ResetRaid25Heroic := 1;
SET @ResetBoss := 1;             -- boss encounters: defeated, or wiped on after CombatTimeRequired
SET @ResetWorldBoss := 1;
SET @ResetElite := 0;            -- when one dies
SET @ResetRare := 0;             -- when one dies
SET @ResetBattleground := 0;     -- when the gates open, or on joining one under way
SET @ResetArena := 0;            -- on entering one, on top of the core's own reset
SET @ResetDuel := 0;             -- when a duel ends: only the cooldowns used in it

-- Where a spell row applies (add them up: 15 = everywhere)
SET @Instance := 1;
SET @Battleground := 2;
SET @Arena := 4;
SET @Duel := 8;

CREATE TABLE IF NOT EXISTS `dynamic_scaling_cooldown_rule` (
    `rule` VARCHAR(32) NOT NULL COMMENT 'Dungeon, DungeonHeroic, Raid10, Raid25, Raid10Heroic, Raid25Heroic, Boss, ...',
    `reset` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 resets cooldowns, 0 not',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`rule`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: cooldown reset rules';

-- type 0: a creature entry. 1 = its death, or a wipe on it after CombatTimeRequired, resets cooldowns (for bosses
--         without a boss encounter script); 0 = it never does.
-- type 1: an encounter, by credit entry or DungeonEncounter.dbc ID. 0 = completing it doesn't reset cooldowns.
CREATE TABLE IF NOT EXISTS `dynamic_scaling_cooldown_reset` (
    `type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 creature entry, 1 encounter',
    `entry` INT UNSIGNED NOT NULL,
    `reset` TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 resets cooldowns, 0 never',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`type`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: cooldown reset creatures and encounters';

-- type 0: a spell ID, type 1: a spell cooldown category (Spell.dbc Category). reset_in: where it is always reset,
-- even at 10 minutes or more; never_in: where it is never reset. Both add up 1 instance, 2 battleground, 4 arena,
-- 8 duel. A spell's own row wins over its category's. Everything else under 10 minutes is reset with
-- DynamicScaling.CooldownReset.ShortCooldowns. In arenas, never_in can't undo the core's own reset on entering.
CREATE TABLE IF NOT EXISTS `dynamic_scaling_cooldown_spell` (
    `type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 spell, 1 category',
    `id` INT UNSIGNED NOT NULL,
    `reset_in` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'always reset: 1 instance, 2 battleground, 4 arena, 8 duel',
    `never_in` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'never reset: 1 instance, 2 battleground, 4 arena, 8 duel',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`type`, `id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: spells always or never reset';

DELETE FROM `dynamic_scaling_cooldown_rule` WHERE `rule` IN ('Dungeon', 'DungeonHeroic', 'Raid10', 'Raid25',
    'Raid10Heroic', 'Raid25Heroic', 'Boss', 'WorldBoss', 'Elite', 'Rare', 'Battleground', 'Arena', 'Duel');
INSERT INTO `dynamic_scaling_cooldown_rule` (`rule`, `reset`, `comment`) VALUES
('Dungeon', @ResetDungeon, 'Normal dungeons'),
('DungeonHeroic', @ResetDungeonHeroic, 'Heroic dungeons'),
('Raid10', @ResetRaid10, '10 player raids'),
('Raid25', @ResetRaid25, '25 player raids (and 20 / 40 player raids)'),
('Raid10Heroic', @ResetRaid10Heroic, '10 player heroic raids'),
('Raid25Heroic', @ResetRaid25Heroic, '25 player heroic raids'),
('Boss', @ResetBoss, 'Boss encounters'),
('WorldBoss', @ResetWorldBoss, 'World bosses'),
('Elite', @ResetElite, 'Elites (when they die)'),
('Rare', @ResetRare, 'Rares (when they die)'),
('Battleground', @ResetBattleground, 'Battlegrounds (gates open, or joining one under way)'),
('Arena', @ResetArena, 'Arenas (on entering)'),
('Duel', @ResetDuel, 'Duels (when one ends, the cooldowns used in it)');

-- By sogladev: long cooldowns also reset after instance encounters
DELETE FROM `dynamic_scaling_cooldown_spell` WHERE (`type` = 0 AND `id` IN (42650, 2062, 2894, 1122))
    OR (`type` = 1 AND `id` = 26);
INSERT INTO `dynamic_scaling_cooldown_spell` (`type`, `id`, `reset_in`, `never_in`, `comment`) VALUES
(0, 42650, @Instance, 0, 'Army of the Dead'),
(0, 2062, @Instance, 0, 'Earth Elemental Totem'),
(0, 2894, @Instance, 0, 'Fire Elemental Totem'),
(0, 1122, @Instance, 0, 'Inferno'),
(1, 26, @Instance, 0, 'Rebirth category');

-- By sogladev: raid bosses without a boss encounter script, and the Obsidian Sanctum
-- drake encounters, which don't reset on their own
DELETE FROM `dynamic_scaling_cooldown_reset` WHERE (`type` = 0 AND `entry` IN (28859, 31125, 31734, 32845, 32846,
    32865, 32871, 32906, 32930, 33070, 33113, 33118, 33147, 33186, 33190, 33271, 33288, 33293, 33360, 33432, 33449,
    33515, 33724, 33885, 33909, 33955, 33993, 33994, 34003, 34106, 34175, 34497, 34564, 34566, 34780, 34797, 35013,
    35216, 35268, 35269, 35350, 35351, 35352, 35360, 35447, 35448, 35449, 35615, 35616, 38462, 12397, 17711, 18728))
    OR (`type` = 1 AND `entry` BETWEEN 736 AND 743);
INSERT INTO `dynamic_scaling_cooldown_reset` (`type`, `entry`, `reset`, `comment`) VALUES
(0, 28859, 1, 'Malygos'),
(0, 31125, 1, 'Archavon the Stone Watcher'),
(0, 31734, 1, 'Malygos (1)'),
(0, 32845, 1, 'Hodir'),
(0, 32846, 1, 'Hodir (1)'),
(0, 32865, 1, 'Thorim'),
(0, 32871, 1, 'Algalon the Observer'),
(0, 32906, 1, 'Freya'),
(0, 32930, 1, 'Kologarn'),
(0, 33070, 1, 'Algalon the Observer (1)'),
(0, 33113, 1, 'Flame Leviathan'),
(0, 33118, 1, 'Ignis the Furnace Master'),
(0, 33147, 1, 'Thorim (1)'),
(0, 33186, 1, 'Razorscale'),
(0, 33190, 1, 'Ignis the Furnace Master (1)'),
(0, 33271, 1, 'General Vezax'),
(0, 33288, 1, 'Yogg-Saron'),
(0, 33293, 1, 'XT-002 Deconstructor'),
(0, 33360, 1, 'Freya (1)'),
(0, 33432, 1, 'Leviathan Mk II'),
(0, 33449, 1, 'General Vezax (1)'),
(0, 33515, 1, 'Auriaya'),
(0, 33724, 1, 'Razorscale (1)'),
(0, 33885, 1, 'XT-002 Deconstructor (1)'),
(0, 33909, 1, 'Kologarn (1)'),
(0, 33955, 1, 'Yogg-Saron (1)'),
(0, 33993, 1, 'Emalon the Storm Watcher'),
(0, 33994, 1, 'Emalon the Storm Watcher (1)'),
(0, 34003, 1, 'Flame Leviathan (1)'),
(0, 34106, 1, 'Leviathan Mk II (1)'),
(0, 34175, 1, 'Auriaya (1)'),
(0, 34497, 1, 'Fjola Lightbane'),
(0, 34564, 1, 'Anub''arak'),
(0, 34566, 1, 'Anub''arak (1)'),
(0, 34780, 1, 'Lord Jaraxxus'),
(0, 34797, 1, 'Icehowl'),
(0, 35013, 1, 'Koralon the Flame Watcher'),
(0, 35216, 1, 'Lord Jaraxxus (1)'),
(0, 35268, 1, 'Lord Jaraxxus (2)'),
(0, 35269, 1, 'Lord Jaraxxus (3)'),
(0, 35350, 1, 'Fjola Lightbane (1)'),
(0, 35351, 1, 'Fjola Lightbane (2)'),
(0, 35352, 1, 'Fjola Lightbane (3)'),
(0, 35360, 1, 'Koralon the Flame Watcher (1)'),
(0, 35447, 1, 'Icehowl (1)'),
(0, 35448, 1, 'Icehowl (2)'),
(0, 35449, 1, 'Icehowl (3)'),
(0, 35615, 1, 'Anub''arak (2)'),
(0, 35616, 1, 'Anub''arak (3)'),
(0, 38462, 1, 'Toravon the Ice Watcher (1)'),
(0, 12397, 1, 'Lord Kazzak'),
(0, 17711, 1, 'Doomwalker'),
(0, 18728, 1, 'Doom Lord Kazzak'),
(1, 736, 0, 'The Obsidian Sanctum - Tenebron'),
(1, 737, 0, 'The Obsidian Sanctum - Tenebron'),
(1, 738, 0, 'The Obsidian Sanctum - Shadron'),
(1, 739, 0, 'The Obsidian Sanctum - Shadron'),
(1, 740, 0, 'The Obsidian Sanctum - Vesperon'),
(1, 741, 0, 'The Obsidian Sanctum - Vesperon'),
(1, 742, 0, 'The Obsidian Sanctum - Sartharion'),
(1, 743, 0, 'The Obsidian Sanctum - Sartharion');
