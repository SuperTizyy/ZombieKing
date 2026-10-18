-- v3.41 实测 nk.matchCreate 返回的 match_id 为 "uuid.<node>" 格式（含节点后缀），uuid 列无法存储
ALTER TABLE game.room ALTER COLUMN match_id TYPE text USING match_id::text;
