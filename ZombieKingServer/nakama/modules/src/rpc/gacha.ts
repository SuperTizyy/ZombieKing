// 开箱（红线 R2 + §4.3.4）：服务端按池配置掷点（含保底），结果传入 fn_gacha_draw 原子落库

import { parseInput, requireIdem, callFn, failIfErr, weightedPick, parseJson } from "../common/api";
import { guardRate } from "../common/rateLimit";

interface PoolItem { item_def_id: string; weight: number; rarity: number; }
interface DrawResult { item_def_id: string; rarity: number; }
interface PityCfg { legendary_at?: number; epic_at?: number; }

export const rpcGachaDraw: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_gacha_draw", ctx.userId, "rate_asset_per_min", 30); }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const poolId = String(input.pool_id || "");
  const count = parseInt(String(input.count), 10);
  if (!poolId || (count !== 1 && count !== 10)) {
    throw new Error("INVALID_INPUT: pool_id/count(1|10)");
  }

  const pools = nk.sqlQuery(
    "SELECT currency, cost, items_json, pity FROM game.gacha_pool WHERE id=$1 AND enabled",
    [poolId]
  );
  if (!pools.length) { throw new Error("POOL_UNAVAILABLE"); }
  const pool = pools[0];
  const items: PoolItem[] = parseJson<PoolItem[]>(pool.items_json, []);
  const pityCfg = parseJson<PityCfg>(pool.pity, {});
  const currency = String(pool.currency);
  const unitCost = parseInt(String(pool.cost), 10);

  const pityRows = nk.sqlQuery(
    "SELECT legendary, epic FROM game.gacha_pity WHERE user_id=$1 AND pool_id=$2",
    [ctx.userId, poolId]
  );
  let leg = pityRows.length ? parseInt(String(pityRows[0].legendary), 10) : 0;
  let epic = pityRows.length ? parseInt(String(pityRows[0].epic), 10) : 0;

  const legItems: PoolItem[] = [];
  const epicItems: PoolItem[] = [];
  for (let i = 0; i < items.length; i++) {
    if (items[i].rarity >= 3) { legItems.push(items[i]); }
    if (items[i].rarity >= 2) { epicItems.push(items[i]); }
  }

  const results: DrawResult[] = [];
  for (let n = 0; n < count; n++) {
    leg += 1; epic += 1;
    let picked: PoolItem;
    if (pityCfg.legendary_at && leg >= pityCfg.legendary_at && legItems.length > 0) {
      picked = weightedPick(nk, legItems);          // 传说保底
    } else if (pityCfg.epic_at && epic >= pityCfg.epic_at && epicItems.length > 0) {
      picked = weightedPick(nk, epicItems);         // 史诗保底
    } else {
      picked = weightedPick(nk, items);
    }
    if (picked.rarity >= 3) { leg = 0; epic = 0; }
    else if (picked.rarity >= 2) { epic = 0; }
    results.push({ item_def_id: picked.item_def_id, rarity: picked.rarity });
  }

  const seed = nk.uuidv4();
  const res = failIfErr(callFn(nk, "fn_gacha_draw", [
    ctx.userId, poolId, currency, unitCost * count,
    JSON.stringify(results), seed,
    JSON.stringify({ legendary: leg, epic: epic }), idem,
  ]));
  logger.info("gacha_draw", "user", ctx.userId, "pool", poolId, "count", count);
  return JSON.stringify(res);
};
