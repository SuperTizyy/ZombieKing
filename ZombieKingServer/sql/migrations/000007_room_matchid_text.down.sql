ALTER TABLE game.room ALTER COLUMN match_id TYPE uuid USING NULLIF(match_id,'')::uuid;
