// 购买（红线 R2）：TS 查配置表定价 → fn_shop_buy 单事务扣发

import { parseInput, requireIdem, callFn, failIfErr } from "../common/api";
import { guardRate } from "../common/rateLimit";

export const rpcShopBuy: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_shop_buy", ctx.userId, "rate_asset_per_min", 30); }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const itemId = String(input.item_def_id || "");
  const qty = parseInt(String(input.qty), 10);
  const currency = String(input.currency || "gold");
  if (!itemId || !(qty >= 1 && qty <= 99)) {
    throw new Error("INVALID_INPUT: item_def_id/qty");
  }

  // 价格只认服务端配置表（客户端任何价格字段忽略）
  const defs = nk.sqlQuery(
    "SELECT price_json->>$1 AS unit FROM game.item_def WHERE id=$2 AND enabled",
    [currency, itemId]
  );
  if (!defs.length || defs[0].unit === null || defs[0].unit === undefined) {
    throw new Error("ITEM_UNAVAILABLE");
  }
  const amount = parseInt(String(defs[0].unit), 10) * qty;

  const res = failIfErr(callFn(nk, "fn_shop_buy", [ctx.userId, itemId, qty, currency, amount, idem]));
  logger.info("shop_buy", "user", ctx.userId, "item", itemId, "qty", qty);
  return JSON.stringify(res);
};
