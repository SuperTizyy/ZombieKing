-- RPC 频控限流（PG 原子固定窗口；goja 多 worker 内存不共享，必须落库计数）
CREATE TABLE IF NOT EXISTS game.rate_limit (
    key          text NOT NULL,                     -- "<rpc_id>:<user_id>"
    window_start timestamptz NOT NULL,              -- date_trunc('minute', now())
    count        integer NOT NULL DEFAULT 1,
    PRIMARY KEY (key, window_start)
);

INSERT INTO game.sys_config(key,value,note) VALUES
    ('rate_asset_per_min','30','asset/room/queue RPC per-user limit per minute'),
    ('rate_lobby_per_min','120','lobby heartbeat/list/channel/queue_status per-user limit per minute'),
    ('rate_account_per_min','5','account bind per-user limit per minute')
ON CONFLICT (key) DO NOTHING;
