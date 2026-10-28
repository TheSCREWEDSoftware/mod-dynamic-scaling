-- mod-dynamic-scaling: open world creatures that always (1) or never (0) scale, whatever the config says.
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

INSERT IGNORE INTO `dynamic_scaling_category` (`category`, `scale`, `comment`) VALUES
('NpcFlags', 0, 'Vendors, quest givers, trainers, flight masters'),
('Civilian', 0, 'Civilians'),
('Guard', 0, 'Guards'),
('PvP', 0, 'Friendly to Alliance or Horde players'),
('Unattackable', 0, 'Can''t be attacked or selected'),
('Elite', 1, 'Elite and rare elite'),
('Rare', 1, 'Rare and rare elite'),
('WorldBoss', 0, 'World bosses'),
('Boss', 0, 'Dungeon bosses');
