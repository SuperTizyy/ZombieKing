-- ZombieKing P6 大厅域：在线列表心跳表 + 房间组队 + 撮合队列整队(party)支持
CREATE TABLE IF NOT EXISTS game.online_users (
    user_id    uuid PRIMARY KEY,
    username   text NOT NULL DEFAULT '',
    last_seen  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_online_users_last_seen ON game.online_users (last_seen);

-- 房间（队长发起，成员经邀请/知道 room_id 加入；开局时全员写入 match_queue 同 party_id）
CREATE TABLE IF NOT EXISTS game.room (
    room_id      uuid PRIMARY KEY,
    owner_id     uuid NOT NULL,
    mode         text NOT NULL,
    map          text NOT NULL DEFAULT '',
    max_members  integer NOT NULL DEFAULT 10,
    match_id     uuid,
    created_at   timestamptz NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS game.room_member (
    room_id   uuid NOT NULL REFERENCES game.room(room_id) ON DELETE CASCADE,
    user_id   uuid NOT NULL,
    joined_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (room_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_room_member_user ON game.room_member (user_id);

-- 撮合队列加 party 列：同 room 成员整队成组
ALTER TABLE game.match_queue ADD COLUMN IF NOT EXISTS party_id uuid;
