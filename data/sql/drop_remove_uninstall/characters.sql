-- mod-dynamic-scaling drop / remove / uninstall, characters database. Read the notes in world.sql first.

-- The custom spell IDs, if you changed them in the config
SET @SimpleBuffSpellID := 150100; -- DynamicScaling.Instance.Simple.SpellId

-- Who turned scaling on or off (.scaling)
DROP TABLE IF EXISTS `dynamic_scaling_player`;

-- The Simple mode buff, if a character was saved with it
DELETE FROM `character_aura` WHERE `spell` = @SimpleBuffSpellID;
DELETE FROM `pet_aura` WHERE `spell` = @SimpleBuffSpellID;

-- Forget the applied file, so a later reinstall runs it again
DELETE FROM `updates` WHERE `state` = 'MODULE' AND `name` IN ('01_dynamic_scaling_player.sql',
    'dynamic_scaling_player.sql');
