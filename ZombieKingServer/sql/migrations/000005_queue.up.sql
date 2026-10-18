-- ZombieKing P5：自研撮合队列（内置 matchmaker 回调在 v3.41 未触发，改用可控的队列表+处理器 Match）
CREATE TABLE IF NOT EXISTS game.match_queue (
    user_id    uuid PRIMARY KEY,
    mode       text NOT NULL,
    mmr        integer NOT NULL DEFAULT 1000,
    queued_at  timestamptz NOT NULL DEFAULT now(),
    match_id   uuid,
    join_token text
);
CREATE INDEX IF NOT EXISTS idx_match_queue_mode_mmr ON game.match_queue (mode, mmr);

INSERT INTO game.sys_config(key,value,note) VALUES
    ('queue_min_tdm','2','tdm_5v5 min players to form (dev=2, prod=10)'),
    ('queue_min_zombie','2','zombie_infection min players to form (dev=2)'),
    ('queue_mmr_window','150','base mmr spread window, widens +50 per 10s waited')
ON CONFLICT (key) DO NOTHING;
