-- 服务端开关配置（RPC ctx 在 v3.41 不暴露 environment，DEV_MODE 门控改用 DB 开关；生产默认 false）
CREATE TABLE IF NOT EXISTS game.sys_config (
    key   text PRIMARY KEY,
    value text NOT NULL,
    note  text NOT NULL DEFAULT ''
);
INSERT INTO game.sys_config(key,value,note) VALUES ('dev_mode','false','GM 加币等开发功能门控；仅本地开发置 true')
ON CONFLICT (key) DO NOTHING;
