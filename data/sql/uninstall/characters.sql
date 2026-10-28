-- mod-dynamic-scaling uninstall, characters database. Read the notes in world.sql first.

-- Who turned scaling on or off (.scaling)
DROP TABLE IF EXISTS `dynamic_scaling_player`;

-- The Simple mode buff, if a character was saved with it
DELETE FROM `character_aura` WHERE `spell` = 150100;
DELETE FROM `pet_aura` WHERE `spell` = 150100;

-- Forget the applied file, so a later reinstall runs it again
DELETE FROM `updates` WHERE `state` = 'MODULE' AND `name` = 'dynamic_scaling_player.sql';
