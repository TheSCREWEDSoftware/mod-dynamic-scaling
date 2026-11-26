-- mod-dynamic-scaling (2026-11-26): one-off cleanup of rows left by characters deleted before the character-delete hook
-- existed (rows whose character is no longer in `characters`). Re-runnable and strict-mode safe: a table that
-- isn't there (yet) is skipped. A character only unlinked (CharDelete.Method = 1) is still in `characters` and keeps
-- its rows, as with the hook.
SET @t := (SELECT COUNT(*) FROM information_schema.TABLES WHERE `TABLE_SCHEMA` = DATABASE() AND `TABLE_NAME` = 'dynamic_scaling_player');
SET @sql := IF(@t > 0, 'DELETE x FROM `dynamic_scaling_player` x LEFT JOIN `characters` c ON c.`guid` = x.`guid` WHERE c.`guid` IS NULL', 'DO 0');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
