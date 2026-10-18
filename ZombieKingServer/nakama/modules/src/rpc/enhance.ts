// 强化（红线 R2）：TS 查规则/选材料/掷判定 → fn_item_enhance 单事务消耗与落级

import { parseInput, requireIdem, callFn, failIfErr, rollFloat, parseJson } from "../common/api";
import { guardRate } from "../common/rateLimit";

interface MatReq { item_def_id: string; count: number; }

export const rpcItemEnhance: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_item_enhance", ctx.userId, "rate_asset_per_min", 30); }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const itemId = parseInt(String(input.inventory_item_id), 10);
  if (!(itemId > 0)) { throw new Error("INVALID_INPUT: inventory_item_id"); }

  const items = nk.sqlQuery(
    "SELECT i.level, d.type FROM game.inventory i JOIN game.item_def d ON d.id=i.item_def_id " +
    "WHERE i.id=$1 AND i.user_id=$2 AND i.status IN ('in_bag','equipped')",
    [itemId, ctx.userId]
  );
  if (!items.length) { throw new Error("ITEM_STATE_INVALID"); }
  const level = parseInt(String(items[0].level), 10);

  const rules = nk.sqlQuery(
    "SELECT to_level, success_rate, cost_json, mats_json, fail_penalty FROM game.enhance_rule " +
    "WHERE target_type=$1 AND from_level=$2 AND enabled ORDER BY to_level LIMIT 1",
    [String(items[0].type), level]
  );
  if (!rules.length) { throw new Error("MAX_LEVEL"); }
  const rule = rules[0];

  // 收集材料实例（函数内再次校验归属/状态并原子消耗，并发冲突整体回滚）
  const matReqs: MatReq[] = parseJson<MatReq[]>(rule.mats_json, []);
  const matIds: number[] = [];
  for (let m = 0; m < matReqs.length; m++) {
    const req = matReqs[m];
    const found = nk.sqlQuery(
      "SELECT id FROM game.inventory WHERE user_id=$1 AND item_def_id=$2 AND status='in_bag' ORDER BY id LIMIT $3",
      [ctx.userId, req.item_def_id, req.count]
    );
    if (found.length < req.count) { throw new Error("MATERIALS_SHORT: " + req.item_def_id); }
    for (let f = 0; f < found.length; f++) { matIds.push(parseInt(String(found[f].id), 10)); }
  }

  const cost = parseJson<{ currency: string; amount: number }>(rule.cost_json, { currency: "gold", amount: 0 });
  const success = rollFloat(nk) < parseFloat(String(rule.success_rate));
  const penalty = String(rule.fail_penalty);
  const newLevel = success
    ? parseInt(String(rule.to_level), 10)
    : (penalty === "downgrade" ? Math.max(0, level - 1) : level);

  const res = failIfErr(callFn(nk, "fn_item_enhance", [
    ctx.userId, itemId, level, newLevel,
    JSON.stringify(matIds), cost.currency, cost.amount || 0, idem,
  ]));
  res.success = success;
  logger.info("item_enhance", "user", ctx.userId, "item", itemId, "success", success);
  return JSON.stringify(res);
};
