// GM/测试加币（§12-1：本期不做真实支付）
// 双重门控：管理员角色 + DEV_MODE 环境变量；强制审计与流水（fn_gm_grant 内完成）

import { parseInput, requireIdem, requireUuid, callFn, failIfErr } from "../common/api";

export const rpcGmGrantCurrency: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  // DEV_MODE 门控：优先 ctx.environment（match 钩子可用），RPC ctx 在 v3.41 无 environment，
  // 回落到 game.sys_config('dev_mode')；生产环境该行为 false 即关闭。
  let devMode = "false";
  const env = (ctx as any).environment;
  if (env && String(env.DEV_MODE) === "true") {
    devMode = "true";
  } else {
    const cfg = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key='dev_mode'");
    if (cfg.length) { devMode = String(cfg[0].value); }
  }
  if (devMode !== "true") {
    throw new Error("DEV_MODE_REQUIRED");
  }

  // v3.41 JS 运行时无 getAccountDetails；服务端同库直读 users.metadata.roles（权威）
  let roles: string[] = [];
  try {
    const rows = nk.sqlQuery("SELECT metadata->>'roles' AS roles FROM users WHERE id=$1", [ctx.userId]);
    roles = rows.length && rows[0].roles ? JSON.parse(String(rows[0].roles)) : [];
  } catch (e) {
    roles = [];
  }
  if (roles.indexOf("admin") < 0) {
    throw new Error("ADMIN_REQUIRED");
  }

  const input = parseInput(params);
  const idem = requireIdem(input);
  const target = requireUuid(input.user_id || ctx.userId || "", "user_id");
  const currency = String(input.currency || "");
  const amount = parseInt(String(input.amount), 10);
  if (!currency || !(amount >= 1 && amount <= 1000000)) {
    throw new Error("INVALID_INPUT: currency/amount(1..1000000)");
  }

  const res = failIfErr(callFn(nk, "fn_gm_grant", [target, currency, amount, ctx.username || ctx.userId, idem]));
  logger.warn("gm_grant", "operator", ctx.username, "target", target, "currency", currency, "amount", amount);
  return JSON.stringify(res);
};
