-- mod-dynamic-scaling: example map rows. Instances with no row use the config defaults, so this file
-- only shows the options. Delete or edit the rows you don't want.

-- The Deadmines: enter at any level, creatures scale with the group between level 10 and 80.
-- Wailing Caverns: Advanced mode (creature health / damage by player count), entry from level 10.
-- Onyxia's Lair 10/25: no level scaling, respawn at the given spot instead of the entrance.
DELETE FROM `dynamic_scaling_map` WHERE `map_id` IN (36, 43, 249);
INSERT INTO `dynamic_scaling_map` (`map_id`, `difficulty`, `mode`, `entry_level_min`, `level_scaling`, `level_min`, `level_max`, `base_level`, `expected_players`, `simple_mult`, `health_mult`, `damage_mult`, `mana_mult`, `armor_mult`, `respawn`, `respawn_x`, `respawn_y`, `respawn_z`, `respawn_o`, `trail_levels`, `xp_min_pct`, `comment`) VALUES
(36, 255, 1, 1, 1, 10, 80, 0, 0, -1, 1, 1, 1, 1, 255, 0, 0, 0, 0, -1, -1, 'The Deadmines - any level, scales 10-80'),
(43, 255, 2, 10, 1, 10, 80, 0, 0, -1, 1, 1, 1, 1, 255, 0, 0, 0, 0, -1, -1, 'Wailing Caverns - advanced, entry 10+'),
(249, 255, 1, 0, 0, 0, 0, 0, 0, -1, 1, 1, 1, 1, 1, 29.17, -182.04, -87.0, 6.25, -1, -1, 'Onyxia''s Lair - no level scaling, manual respawn point (example coords, check with .gps)');
