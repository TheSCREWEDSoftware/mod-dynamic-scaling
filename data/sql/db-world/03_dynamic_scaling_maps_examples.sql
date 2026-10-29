-- mod-dynamic-scaling: example map rows. Instances with no row use the config defaults, so this file
-- only shows the options. Change the values below, or delete the rows you don't want.

-- The Deadmines: enter at any level, creatures scale with the group between level 10 and 80
SET @DeadminesMapID := 36;
SET @DeadminesEntryLevel := 1;
SET @DeadminesLevelMin := 10;
SET @DeadminesLevelMax := 80;

-- Wailing Caverns: Advanced mode (creature health / damage by player count), entry from level 10
SET @WailingCavernsMapID := 43;
SET @WailingCavernsEntryLevel := 10;
SET @WailingCavernsLevelMin := 10;
SET @WailingCavernsLevelMax := 80;

-- Onyxia's Lair 10/25: no level scaling, respawn at the given spot instead of the entrance (example coordinates,
-- check them with .gps)
SET @OnyxiaMapID := 249;
SET @OnyxiaRespawnX := 29.17;
SET @OnyxiaRespawnY := -182.04;
SET @OnyxiaRespawnZ := -87.0;
SET @OnyxiaRespawnO := 6.25;

DELETE FROM `dynamic_scaling_map` WHERE `map_id` IN (@DeadminesMapID, @WailingCavernsMapID, @OnyxiaMapID);
INSERT INTO `dynamic_scaling_map` (`map_id`, `difficulty`, `mode`, `entry_level_min`, `level_scaling`, `level_min`, `level_max`, `base_level`, `expected_players`, `simple_mult`, `health_mult`, `damage_mult`, `mana_mult`, `armor_mult`, `respawn`, `respawn_x`, `respawn_y`, `respawn_z`, `respawn_o`, `trail_levels`, `xp_min_pct`, `comment`) VALUES
(@DeadminesMapID, 255, 1, @DeadminesEntryLevel, 1, @DeadminesLevelMin, @DeadminesLevelMax, 0, 0, -1, 1, 1, 1, 1, 255, 0, 0, 0, 0, -1, -1, 'The Deadmines - any level, scales 10-80'),
(@WailingCavernsMapID, 255, 2, @WailingCavernsEntryLevel, 1, @WailingCavernsLevelMin, @WailingCavernsLevelMax, 0, 0, -1, 1, 1, 1, 1, 255, 0, 0, 0, 0, -1, -1, 'Wailing Caverns - advanced, entry 10+'),
(@OnyxiaMapID, 255, 1, 0, 0, 0, 0, 0, 0, -1, 1, 1, 1, 1, 1, @OnyxiaRespawnX, @OnyxiaRespawnY, @OnyxiaRespawnZ, @OnyxiaRespawnO, -1, -1, 'Onyxia''s Lair - no level scaling, manual respawn point');
