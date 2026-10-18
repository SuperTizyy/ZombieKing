DROP TABLE IF EXISTS game.match_queue;
DELETE FROM game.sys_config WHERE key IN ('queue_min_tdm','queue_min_zombie','queue_mmr_window');
