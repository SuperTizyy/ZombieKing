-- ZombieKing P2：game.fn_* PL/pgSQL 存储过程（多表事务唯一入口，架构文档 §5.1）
-- 约定：函数只做"参数已算好"的原子写，返回 jsonb（ok/err）；随机数/价格/概率在 TS 层。

-- 1) 物品状态增加 consumed（强化材料消耗）
ALTER TABLE game.inventory DROP CONSTRAINT IF EXISTS inventory_status_check;
ALTER TABLE game.inventory ADD CONSTRAINT inventory_status_check
    CHECK (status IN ('in_bag','equipped','consumed'));

-- 2) 开箱保底计数器（服务端权威，随抽取事务更新）
CREATE TABLE IF NOT EXISTS game.gacha_pity (
    user_id   uuid NOT NULL,
    pool_id   text NOT NULL REFERENCES game.gacha_pool(id),
    legendary integer NOT NULL DEFAULT 0,
    epic      integer NOT NULL DEFAULT 0,
    PRIMARY KEY (user_id, pool_id)
);
ALTER TABLE game.gacha_log DROP COLUMN IF EXISTS pity_after;

-- ============ 购买 ============
CREATE OR REPLACE FUNCTION game.fn_shop_buy(
    p_user uuid, p_item_def_id text, p_qty int,
    p_currency text, p_amount bigint, p_idem uuid
) RETURNS jsonb AS $$
DECLARE
    v_balance bigint;
    v_replay  jsonb;
    v_price   bigint;
    v_inv     bigint[];
    v_ledger  bigint;
BEGIN
    -- 幂等命中：返回首次结果
    SELECT jsonb_build_object('ok',true,'replayed',true,'ledger_id',l.id,'balance',w.balance)
      INTO v_replay
      FROM game.ledger l
      JOIN game.wallet w ON w.user_id=l.user_id AND w.currency=l.currency
     WHERE l.idempotency_key = p_idem;
    IF FOUND THEN RETURN v_replay; END IF;

    -- 防御性复核：在售 + 价格与配置表一致（即使 TS 被绕过也无法低价买入）
    SELECT (d.price_json ->> p_currency)::bigint * p_qty INTO v_price
      FROM game.item_def d
     WHERE d.id = p_item_def_id AND d.enabled;
    IF NOT FOUND THEN RETURN jsonb_build_object('err','ITEM_UNAVAILABLE'); END IF;
    IF v_price IS NULL OR v_price <> p_amount THEN RETURN jsonb_build_object('err','PRICE_MISMATCH'); END IF;

    INSERT INTO game.wallet(user_id,currency,balance) VALUES (p_user,p_currency,0)
    ON CONFLICT DO NOTHING;

    UPDATE game.wallet
       SET balance=balance-p_amount, version=version+1, updated_at=now()
     WHERE user_id=p_user AND currency=p_currency AND balance>=p_amount
    RETURNING balance INTO v_balance;
    IF NOT FOUND THEN RETURN jsonb_build_object('err','INSUFFICIENT_FUNDS'); END IF;

    WITH ins AS (
      INSERT INTO game.inventory(user_id,item_def_id)
      SELECT p_user, p_item_def_id FROM generate_series(1,p_qty) RETURNING id
    ) SELECT array_agg(id) INTO v_inv FROM ins;

    INSERT INTO game.ledger(user_id,kind,currency,delta,ref_id,idempotency_key)
    VALUES (p_user,'purchase',p_currency,-p_amount,p_item_def_id,p_idem)
    RETURNING id INTO v_ledger;

    RETURN jsonb_build_object('ok',true,'balance',v_balance,'inventory_ids',to_jsonb(v_inv),'ledger_id',v_ledger);
END $$ LANGUAGE plpgsql;

-- ============ 开箱（结果由 TS 掷好传入） ============
CREATE OR REPLACE FUNCTION game.fn_gacha_draw(
    p_user uuid, p_pool_id text, p_currency text, p_amount bigint,
    p_results jsonb, p_server_seed text, p_pity jsonb, p_idem uuid
) RETURNS jsonb AS $$
DECLARE
    v_balance bigint;
    v_replay  jsonb;
    v_cost    bigint;
    v_cur     text;
    v_inv     bigint[];
    v_ledger  bigint;
    v_log     bigint;
BEGIN
    SELECT jsonb_build_object('ok',true,'replayed',true,'ledger_id',l.id)
      INTO v_replay
      FROM game.ledger l WHERE l.idempotency_key = p_idem;
    IF FOUND THEN RETURN v_replay; END IF;

    SELECT cost, currency INTO v_cost, v_cur
      FROM game.gacha_pool WHERE id=p_pool_id AND enabled
        AND (open_from IS NULL OR open_from<=now())
        AND (open_to IS NULL OR open_to>=now());
    IF NOT FOUND THEN RETURN jsonb_build_object('err','POOL_UNAVAILABLE'); END IF;
    IF v_cur <> p_currency OR v_cost * jsonb_array_length(p_results) <> p_amount THEN
        RETURN jsonb_build_object('err','PRICE_MISMATCH');
    END IF;

    INSERT INTO game.wallet(user_id,currency,balance) VALUES (p_user,p_currency,0)
    ON CONFLICT DO NOTHING;

    UPDATE game.wallet
       SET balance=balance-p_amount, version=version+1, updated_at=now()
     WHERE user_id=p_user AND currency=p_currency AND balance>=p_amount
    RETURNING balance INTO v_balance;
    IF NOT FOUND THEN RETURN jsonb_build_object('err','INSUFFICIENT_FUNDS'); END IF;

    WITH ins AS (
      INSERT INTO game.inventory(user_id,item_def_id,quality)
      SELECT p_user, r->>'item_def_id', (r->>'rarity')::int
        FROM jsonb_array_elements(p_results) AS r RETURNING id
    ) SELECT array_agg(id) INTO v_inv FROM ins;

    INSERT INTO game.gacha_log(user_id,pool_id,results,server_seed_hash,idempotency_key)
    VALUES (p_user,p_pool_id,p_results,p_server_seed,p_idem)
    RETURNING id INTO v_log;

    INSERT INTO game.gacha_pity(user_id,pool_id,legendary,epic)
    VALUES (p_user,p_pool_id,(p_pity->>'legendary')::int,(p_pity->>'epic')::int)
    ON CONFLICT (user_id,pool_id) DO UPDATE
      SET legendary=EXCLUDED.legendary, epic=EXCLUDED.epic;

    INSERT INTO game.ledger(user_id,kind,currency,delta,ref_id,idempotency_key)
    VALUES (p_user,'gacha',p_currency,-p_amount,p_pool_id,p_idem)
    RETURNING id INTO v_ledger;

    RETURN jsonb_build_object('ok',true,'balance',v_balance,'results',p_results,
                              'inventory_ids',to_jsonb(v_inv),'log_id',v_log,'ledger_id',v_ledger);
END $$ LANGUAGE plpgsql;

-- ============ 强化（判定结果由 TS 传入） ============
CREATE OR REPLACE FUNCTION game.fn_item_enhance(
    p_user uuid, p_item_id bigint, p_from_level int, p_new_level int,
    p_mat_ids jsonb, p_currency text, p_amount bigint, p_idem uuid
) RETURNS jsonb AS $$
DECLARE
    v_replay  jsonb;
    v_target  game.inventory%ROWTYPE;
    v_consumed int;
    v_balance bigint;
    v_ledger  bigint;
BEGIN
    SELECT jsonb_build_object('ok',true,'replayed',true,'ledger_id',l.id)
      INTO v_replay
      FROM game.ledger l WHERE l.idempotency_key = p_idem;
    IF FOUND THEN RETURN v_replay; END IF;

    SELECT * INTO v_target FROM game.inventory
     WHERE id=p_item_id AND user_id=p_user AND level=p_from_level
       AND status IN ('in_bag','equipped')
     FOR UPDATE;
    IF NOT FOUND THEN RETURN jsonb_build_object('err','ITEM_STATE_INVALID'); END IF;

    IF jsonb_array_length(p_mat_ids) > 0 THEN
        UPDATE game.inventory SET status='consumed'
         WHERE user_id=p_user AND status='in_bag'
           AND id IN (SELECT (r)::text::bigint FROM jsonb_array_elements(p_mat_ids) AS r);
        GET DIAGNOSTICS v_consumed = ROW_COUNT;
        IF v_consumed <> jsonb_array_length(p_mat_ids) THEN
            RETURN jsonb_build_object('err','MATERIALS_INVALID');  -- 回滚整个事务
        END IF;
    END IF;

    IF p_amount > 0 THEN
        INSERT INTO game.wallet(user_id,currency,balance) VALUES (p_user,p_currency,0)
        ON CONFLICT DO NOTHING;
        UPDATE game.wallet
           SET balance=balance-p_amount, version=version+1, updated_at=now()
         WHERE user_id=p_user AND currency=p_currency AND balance>=p_amount
        RETURNING balance INTO v_balance;
        IF NOT FOUND THEN RETURN jsonb_build_object('err','INSUFFICIENT_FUNDS'); END IF;
    END IF;

    UPDATE game.inventory SET level=p_new_level WHERE id=p_item_id;

    INSERT INTO game.ledger(user_id,kind,currency,delta,ref_id,idempotency_key)
    VALUES (p_user,'enhance',p_currency,-p_amount,p_item_id::text,p_idem)
    RETURNING id INTO v_ledger;

    RETURN jsonb_build_object('ok',true,'item_id',p_item_id,'new_level',p_new_level,
                              'balance',v_balance,'ledger_id',v_ledger);
END $$ LANGUAGE plpgsql;

-- ============ 换装（TS 已校验归属/槽位/模式，函数内复核归属与状态） ============
CREATE OR REPLACE FUNCTION game.fn_loadout_set(
    p_user uuid, p_mode text, p_slots jsonb
) RETURNS jsonb AS $$
DECLARE
    v_old     jsonb;
    v_ok      int;
    v_total   int;
    v_version int;
BEGIN
    SELECT count(*) INTO v_total FROM jsonb_object_keys(p_slots);
    -- 新槽位物品必须全部属于该玩家且处于可用状态
    SELECT count(*) INTO v_ok
      FROM game.inventory
     WHERE user_id=p_user AND status IN ('in_bag','equipped')
       AND id IN (SELECT (r.value #>> '{}')::bigint FROM jsonb_each(p_slots) AS r);
    IF v_ok <> v_total THEN
        RETURN jsonb_build_object('err','ITEM_OWNERSHIP_INVALID');
    END IF;

    -- 旧 loadout 中不在新集合的置回 in_bag
    SELECT slots INTO v_old FROM game.loadout WHERE user_id=p_user AND mode=p_mode;
    IF v_old IS NOT NULL THEN
        UPDATE game.inventory SET status='in_bag'
         WHERE user_id=p_user AND status='equipped'
           AND id NOT IN (SELECT (r.value #>> '{}')::bigint FROM jsonb_each(p_slots) AS r)
           AND id IN (SELECT (r.value #>> '{}')::bigint FROM jsonb_each(v_old) AS r);
    END IF;

    UPDATE game.inventory SET status='equipped'
     WHERE user_id=p_user AND id IN (SELECT (r.value #>> '{}')::bigint FROM jsonb_each(p_slots) AS r);

    INSERT INTO game.loadout(user_id,mode,slots,version,updated_at)
    VALUES (p_user,p_mode,p_slots,1,now())
    ON CONFLICT (user_id,mode) DO UPDATE
      SET slots=EXCLUDED.slots, version=game.loadout.version+1, updated_at=now()
    RETURNING version INTO v_version;

    RETURN jsonb_build_object('ok',true,'version',v_version);
END $$ LANGUAGE plpgsql;

-- ============ GM 加币（管理员+DEV_MODE 门控在 TS 层，函数保证审计） ============
CREATE OR REPLACE FUNCTION game.fn_gm_grant(
    p_user uuid, p_currency text, p_amount bigint, p_operator text, p_idem uuid
) RETURNS jsonb AS $$
DECLARE
    v_replay  jsonb;
    v_balance bigint;
    v_ledger  bigint;
BEGIN
    SELECT jsonb_build_object('ok',true,'replayed',true,'ledger_id',l.id)
      INTO v_replay
      FROM game.ledger l WHERE l.idempotency_key = p_idem;
    IF FOUND THEN RETURN v_replay; END IF;

    IF p_amount <= 0 THEN RETURN jsonb_build_object('err','BAD_AMOUNT'); END IF;
    IF NOT EXISTS (SELECT 1 FROM game.currency_def WHERE code=p_currency) THEN
        RETURN jsonb_build_object('err','CURRENCY_UNKNOWN');
    END IF;

    INSERT INTO game.wallet(user_id,currency,balance) VALUES (p_user,p_currency,p_amount)
    ON CONFLICT (user_id,currency)
      DO UPDATE SET balance=game.wallet.balance+EXCLUDED.balance,
                    version=game.wallet.version+1,
                    updated_at=now()
    RETURNING balance INTO v_balance;

    INSERT INTO game.ledger(user_id,kind,currency,delta,ref_id,idempotency_key)
    VALUES (p_user,'gm_grant',p_currency,p_amount,p_operator,p_idem)
    RETURNING id INTO v_ledger;

    INSERT INTO game.audit_log(actor,action,target,detail)
    VALUES ('gm','grant_currency',p_user::text,
            jsonb_build_object('currency',p_currency,'amount',p_amount,'operator',p_operator));

    RETURN jsonb_build_object('ok',true,'balance',v_balance,'ledger_id',v_ledger);
END $$ LANGUAGE plpgsql;
