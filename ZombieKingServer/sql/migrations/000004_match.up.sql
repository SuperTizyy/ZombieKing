-- ZombieKing P3：结算链路（match_record 本局密钥 + fn_match_settle）

ALTER TABLE game.match_record ADD COLUMN IF NOT EXISTS ds_secret text;

-- 结算入账（TS 复验通过后调用；单事务原子写；幂等由 status 条件更新保证）
CREATE OR REPLACE FUNCTION game.fn_match_settle(
    p_match_id uuid, p_results jsonb, p_rewards jsonb, p_rating jsonb, p_payload_hash text
) RETURNS jsonb AS $$
DECLARE
    v_status text;
BEGIN
    SELECT status INTO v_status FROM game.match_record WHERE match_id = p_match_id FOR UPDATE;
    IF v_status IS NULL THEN RETURN jsonb_build_object('err','MATCH_NOT_FOUND'); END IF;
    IF v_status <> 'pending' THEN RETURN jsonb_build_object('ok',true,'replayed',true); END IF;

    INSERT INTO game.match_result(match_id,user_id,stats,payload_hash)
    SELECT p_match_id, (r->>'user_id')::uuid, r->'stats', p_payload_hash
      FROM jsonb_array_elements(p_results) AS r;

    INSERT INTO game.player_rating(user_id,mode,mmr,wins,losses)
    SELECT (r->>'user_id')::uuid, r->>'mode',
           1000 + (r->>'delta')::int,
           CASE WHEN (r->>'won')::boolean THEN 1 ELSE 0 END,
           CASE WHEN (r->>'won')::boolean THEN 0 ELSE 1 END
      FROM jsonb_array_elements(p_rating) AS r
    ON CONFLICT (user_id,mode) DO UPDATE
      SET mmr = game.player_rating.mmr + EXCLUDED.mmr - 1000,
          wins = game.player_rating.wins + EXCLUDED.wins,
          losses = game.player_rating.losses + EXCLUDED.losses,
          updated_at = now();

    INSERT INTO game.wallet(user_id,currency,balance)
    SELECT (w->>'user_id')::uuid, w->>'currency', (w->>'amount')::bigint
      FROM jsonb_array_elements(p_rewards) AS w
    ON CONFLICT (user_id,currency) DO UPDATE
      SET balance = game.wallet.balance + EXCLUDED.balance,
          version = game.wallet.version + 1,
          updated_at = now();

    INSERT INTO game.ledger(user_id,kind,currency,delta,ref_id)
    SELECT (w->>'user_id')::uuid, 'settle', w->>'currency', (w->>'amount')::bigint, p_match_id::text
      FROM jsonb_array_elements(p_rewards) AS w;

    UPDATE game.match_record SET status='settled', ds_secret=NULL WHERE match_id=p_match_id;

    RETURN jsonb_build_object('ok',true);
END $$ LANGUAGE plpgsql;
