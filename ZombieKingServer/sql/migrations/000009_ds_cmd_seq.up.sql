-- DS Manager 持久化：跨重启单调指令序号（agent 防重放要求严格递增，sys_config.value 有 8KB CHECK 不适合高频更新，独立小表）
CREATE TABLE IF NOT EXISTS game.ds_cmd_seq (
    id    int PRIMARY KEY,
    value bigint NOT NULL
);
-- 基线 1000000：高于旧内存版 manager 历史 seq
INSERT INTO game.ds_cmd_seq(id,value) VALUES (1,1000000) ON CONFLICT (id) DO NOTHING;
