-- mod-dynamic-scaling: per creature and per category rules for the open world.

-- Which kinds of creature scale (1) or not (0), used when DynamicScaling.World.UseDBOverride = 1
SET @ScaleNpcFlags := 0;
SET @ScaleCivilian := 0;
SET @ScaleGuard := 0;
SET @ScalePvP := 0;
SET @ScaleUnattackable := 0;
SET @ScaleElite := 1;
SET @ScaleRare := 1;
SET @ScaleWorldBoss := 0;
SET @ScaleBoss := 0;

-- Creatures that always (1) or never (0) scale, whatever the config says
CREATE TABLE IF NOT EXISTS `dynamic_scaling_creature` (
    `entry` INT UNSIGNED NOT NULL COMMENT 'creature_template.entry',
    `scale` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 never, 1 always',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: per creature rules';

-- Kinds of creature that scale (1) or not (0) in the open world, used instead of DynamicScaling.World.NeverScale
-- when DynamicScaling.World.UseDBOverride = 1. A category with no row scales.
CREATE TABLE IF NOT EXISTS `dynamic_scaling_category` (
    `category` VARCHAR(32) NOT NULL COMMENT 'NpcFlags, Civilian, Guard, PvP, Unattackable, Elite, Rare, WorldBoss, Boss',
    `scale` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 never, 1 scales',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`category`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: per category rules';

DELETE FROM `dynamic_scaling_category` WHERE `category` IN ('NpcFlags', 'Civilian', 'Guard', 'PvP', 'Unattackable',
    'Elite', 'Rare', 'WorldBoss', 'Boss');
INSERT INTO `dynamic_scaling_category` (`category`, `scale`, `comment`) VALUES
('NpcFlags', @ScaleNpcFlags, 'Vendors, quest givers, trainers, flight masters'),
('Civilian', @ScaleCivilian, 'Civilians'),
('Guard', @ScaleGuard, 'Guards'),
('PvP', @ScalePvP, 'Friendly to Alliance or Horde players'),
('Unattackable', @ScaleUnattackable, 'Can''t be attacked or selected'),
('Elite', @ScaleElite, 'Elite and rare elite'),
('Rare', @ScaleRare, 'Rare and rare elite'),
('WorldBoss', @ScaleWorldBoss, 'World bosses'),
('Boss', @ScaleBoss, 'Dungeon bosses');
