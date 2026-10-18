DROP FUNCTION IF EXISTS game.fn_match_settle(uuid, jsonb, jsonb, jsonb, text);
ALTER TABLE game.match_record DROP COLUMN IF EXISTS ds_secret;
