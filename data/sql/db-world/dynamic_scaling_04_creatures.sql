-- mod-dynamic-scaling: open world creatures that always (1) or never (0) scale, whatever the config says.
CREATE TABLE IF NOT EXISTS `dynamic_scaling_creature` (
    `entry` INT UNSIGNED NOT NULL COMMENT 'creature_template.entry',
    `scale` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 never, 1 always',
    `comment` VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (`entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='mod-dynamic-scaling: per creature rules';
