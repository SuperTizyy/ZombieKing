-- ZombieKing game schema：业务表与配置表（依据架构文档 v1.0 §5）
-- 注意：user_id 不加外键，避免与 Nakama 自身建表顺序耦合；一致性由服务端权威逻辑保证。

CREATE SCHEMA IF NOT EXISTS game;

-- ============ 资产域 ============

CREATE TABLE game.currency_def (
    code        text PRIMARY KEY,
    name        text NOT NULL,
    cap         bigint NOT NULL DEFAULT 0          -- 0 = 无上限
);

CREATE TABLE game.wallet (
    user_id     uuid NOT NULL,
    currency    text NOT NULL REFERENCES game.currency_def(code),
    balance     bigint NOT NULL DEFAULT 0 CHECK (balance >= 0),
    version     integer NOT NULL DEFAULT 0,
    updated_at  timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, currency)
);

-- 商品/装备静态配置（运营改表即生效；seed.sql 初始化）
CREATE TABLE game.item_def (
    id          text PRIMARY KEY,
    name        text NOT NULL,
    type        text NOT NULL,                      -- weapon / skin / material / consumable
    rarity      integer NOT NULL DEFAULT 0,         -- 0普通 1稀有 2史诗 3传说
    price_json  jsonb NOT NULL DEFAULT '{}',        -- {"gold":1000,"diamond":10}
    mode_tags   text[] NOT NULL DEFAULT '{}',       -- 适用模式：tdm_5v5 / zombie_infection
    slot        text NOT NULL DEFAULT '',           -- 装备槽位：primary/secondary/melee/skin...
    enabled     boolean NOT NULL DEFAULT true,
    sort        integer NOT NULL DEFAULT 0,
    updated_at  timestamptz NOT NULL DEFAULT now()
);

-- 物品实例（背包）
CREATE TABLE game.inventory (
    id            bigserial PRIMARY KEY,
    user_id       uuid NOT NULL,
    item_def_id   text NOT NULL REFERENCES game.item_def(id),
    quality       integer NOT NULL DEFAULT 0,       -- 强化等级复用 level，quality 预留品质附加
    level         integer NOT NULL DEFAULT 0,
    instance_data jsonb NOT NULL DEFAULT '{}',
    status        text NOT NULL DEFAULT 'in_bag' CHECK (status IN ('in_bag','equipped')),
    created_at    timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_inventory_user ON game.inventory (user_id, status);

-- 配装方案（服务端校验后落库；进局由 join_token 快照锁定）
CREATE TABLE game.loadout (
    user_id    uuid NOT NULL,
    mode       text NOT NULL,
    slots      jsonb NOT NULL DEFAULT '{}',         -- {"primary":invId,"secondary":invId,...}
    version    integer NOT NULL DEFAULT 1,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, mode)
);

-- 资产流水：审计唯一事实源，只增不改不删
CREATE TABLE game.ledger (
    id              bigserial PRIMARY KEY,
    user_id         uuid NOT NULL,
    kind            text NOT NULL CHECK (kind IN ('purchase','gacha','enhance','reward','settle','refund','gm_grant')),
    currency        text NOT NULL REFERENCES game.currency_def(code),
    delta           bigint NOT NULL,                -- 正=发放 负=扣减
    ref_id          text NOT NULL DEFAULT '',       -- 关联业务对象（订单/对局/物品）
    idempotency_key uuid UNIQUE,
    created_at      timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_ledger_user_time ON game.ledger (user_id, created_at DESC);

-- ============ 开箱 / 强化配置 ============

CREATE TABLE game.gacha_pool (
    id          text PRIMARY KEY,
    name        text NOT NULL,
    currency    text NOT NULL REFERENCES game.currency_def(code),
    cost        integer NOT NULL CHECK (cost > 0),
    items_json  jsonb NOT NULL,                    -- [{"item_def_id":"...","weight":100,"rarity":1},...]
    pity        jsonb NOT NULL DEFAULT '{}',       -- {"legendary_at":50,"epic_at":10}
    enabled     boolean NOT NULL DEFAULT true,
    open_from   timestamptz,
    open_to     timestamptz
);

CREATE TABLE game.gacha_log (
    id               bigserial PRIMARY KEY,
    user_id          uuid NOT NULL,
    pool_id          text NOT NULL REFERENCES game.gacha_pool(id),
    results          jsonb NOT NULL,               -- TS 掷好的结果数组
    server_seed_hash text NOT NULL,
    pity_after       integer NOT NULL DEFAULT 0,
    idempotency_key  uuid UNIQUE,
    created_at       timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_gacha_log_user ON game.gacha_log (user_id, created_at DESC);

CREATE TABLE game.enhance_rule (
    id            text PRIMARY KEY,
    target_type   text NOT NULL,                   -- 作用物品类型：weapon...
    from_level    integer NOT NULL,
    to_level      integer NOT NULL,
    success_rate  numeric(6,5) NOT NULL CHECK (success_rate > 0 AND success_rate <= 1),
    cost_json     jsonb NOT NULL DEFAULT '{}',     -- 强化费用 {"currency":"gold","amount":500}
    mats_json     jsonb NOT NULL DEFAULT '[]',     -- 需求材料 [{"item_def_id":"enhance_stone","count":2}]
    fail_penalty  text NOT NULL DEFAULT 'keep' CHECK (fail_penalty IN ('keep','downgrade','destroy')),
    enabled       boolean NOT NULL DEFAULT true
);

-- ============ 撮合 / 对局 ============

CREATE TABLE game.player_rating (
    user_id  uuid NOT NULL,
    mode     text NOT NULL,
    mmr      integer NOT NULL DEFAULT 1000,
    wins     integer NOT NULL DEFAULT 0,
    losses   integer NOT NULL DEFAULT 0,
    streak   integer NOT NULL DEFAULT 0,
    updated_at timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (user_id, mode)
);

-- 结算复验基准（撮合分配 DS 后由 Nakama 写入）
CREATE TABLE game.match_record (
    match_id    uuid PRIMARY KEY,
    mode        text NOT NULL,
    map         text NOT NULL DEFAULT '',
    ds_id       uuid NOT NULL,
    ds_host_id  text NOT NULL,
    player_set  jsonb NOT NULL,                    -- [{"user_id":"...","team":1},...]
    started_at  timestamptz NOT NULL,
    expire_at   timestamptz NOT NULL,
    status      text NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','settled','void')),
    created_at  timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_match_record_status ON game.match_record (status, expire_at);

-- 已复验结算明细：stats 为模式相关 JSONB（tdm_5v5 / zombie_infection 字段集不同）
CREATE TABLE game.match_result (
    match_id     uuid NOT NULL REFERENCES game.match_record(match_id),
    user_id      uuid NOT NULL,
    stats        jsonb NOT NULL,
    payload_hash text NOT NULL DEFAULT '',
    settled_at   timestamptz NOT NULL DEFAULT now(),
    PRIMARY KEY (match_id, user_id)
);

-- ============ DS 农场 ============

CREATE TABLE game.ds_host (
    host_id          text PRIMARY KEY,
    ip               text NOT NULL,
    agent_secret_enc text NOT NULL,                -- host_secret 密文（AES，主密钥走环境变量）
    status           text NOT NULL DEFAULT 'active' CHECK (status IN ('active','isolated','offline')),
    max_ds           integer NOT NULL DEFAULT 8,
    last_heartbeat   timestamptz,
    suspicious_count integer NOT NULL DEFAULT 0
);

CREATE TABLE game.ds_instance (
    ds_id      uuid PRIMARY KEY,
    host_id    text NOT NULL REFERENCES game.ds_host(host_id),
    match_id   uuid,
    port       integer NOT NULL,
    pid        integer,
    status     text NOT NULL DEFAULT 'idle',       -- 状态机：见文档 §4.5.1
    updated_at timestamptz NOT NULL DEFAULT now()
);

-- ============ 审计 ============

CREATE TABLE game.audit_log (
    id         bigserial PRIMARY KEY,
    actor      text NOT NULL,                      -- client / user / ds / manager / gm
    action     text NOT NULL,
    target     text NOT NULL DEFAULT '',
    detail     jsonb NOT NULL DEFAULT '{}',
    created_at timestamptz NOT NULL DEFAULT now()
);
CREATE INDEX idx_audit_time ON game.audit_log (created_at DESC);
