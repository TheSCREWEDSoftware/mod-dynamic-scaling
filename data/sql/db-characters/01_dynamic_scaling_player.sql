-- mod-dynamic-scaling: each player's .scaling choice (enabled: 0 off, 1 Simple, 2 Advanced).
-- No row = DynamicScaling.PlayerToggle.Default.
CREATE TABLE IF NOT EXISTS `dynamic_scaling_player` (
    `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
    `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: player opt in / out';
