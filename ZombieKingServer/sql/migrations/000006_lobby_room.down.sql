ALTER TABLE game.match_queue DROP COLUMN IF EXISTS party_id;
DROP TABLE IF EXISTS game.room_member;
DROP TABLE IF EXISTS game.room;
DROP TABLE IF EXISTS game.online_users;
