-- mod-dynamic-scaling drop / remove / uninstall, world database.
-- Run it only after the module is out of the build (moved out of modules/ or in -DDISABLED_AC_MODULES) and the
-- worldserver was rebuilt; otherwise the DB updater installs everything again on the next start.
-- Everything else the module changes (creature levels and stats, instance entry levels, respawn points) is changed
-- in memory only and is back to the core's values as soon as the module is gone.

-- The module's tables
DROP TABLE IF EXISTS `dynamic_scaling_map`;
DROP TABLE IF EXISTS `dynamic_scaling_zone`;
DROP TABLE IF EXISTS `dynamic_scaling_creature`;
DROP TABLE IF EXISTS `dynamic_scaling_category`;

-- The Simple mode buff (DynamicScaling.Simple.SpellId; change the id if you changed it in the config)
DELETE FROM `spell_dbc` WHERE `ID` = 150100;

-- Forget the applied files, so a later reinstall runs them again
DELETE FROM `updates` WHERE `state` = 'MODULE' AND `name` IN ('01_dynamic_scaling_tables.sql',
    '02_dynamic_scaling_zones.sql', '03_dynamic_scaling_maps_examples.sql', '04_dynamic_scaling_creatures.sql',
    -- the names before the files were numbered first
    'dynamic_scaling_01_tables.sql', 'dynamic_scaling_02_zones.sql', 'dynamic_scaling_03_maps_examples.sql',
    'dynamic_scaling_04_creatures.sql');
