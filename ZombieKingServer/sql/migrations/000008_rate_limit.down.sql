DELETE FROM game.sys_config WHERE key IN ('rate_asset_per_min','rate_lobby_per_min','rate_account_per_min');
DROP TABLE IF EXISTS game.rate_limit;
