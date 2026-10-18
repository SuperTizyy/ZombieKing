// 账号绑定：设备账号 → 邮箱+密码（游客升级正式账号）
// 绑定后客户端可用 Nakama 原生 AuthenticateEmail(email, password) 登录，得到同一 user_id。

import { parseInput } from "../common/api";
import { guardRate } from "../common/rateLimit";

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
const MIN_PWD = 8;
const MAX_PWD = 100;

export const rpcAccountBindEmail: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  guardRate(nk, "rpc_account_bind_email", ctx.userId, "rate_account_per_min", 5);
  const input = parseInput(params);
  const email = String(input.email || "").trim().toLowerCase();
  const password = String(input.password || "");

  if (!EMAIL_RE.test(email)) { throw new Error("INVALID_INPUT: email 格式不正确"); }
  if (password.length < MIN_PWD || password.length > MAX_PWD) {
    throw new Error("INVALID_INPUT: 密码长度需为 " + MIN_PWD + "-" + MAX_PWD + " 位");
  }

  // 已绑定过邮箱的账号不允许重复绑定
  const rows = nk.sqlQuery("SELECT email FROM users WHERE id=$1", [ctx.userId]);
  if (rows.length && rows[0].email) {
    throw new Error("ALREADY_BOUND: 该账号已绑定邮箱");
  }

  try {
    nk.linkEmail(ctx.userId, email, password);
  } catch (e) {
    const msg = String(e && (e as any).message ? (e as any).message : e);
    logger.warn("linkEmail failed", "user", ctx.userId, "err", msg);
    if (/exist|use|conflict/i.test(msg)) { throw new Error("EMAIL_IN_USE: 该邮箱已被使用"); }
    throw new Error("BIND_FAILED: " + msg);
  }

  logger.info("email bound", "user", ctx.userId);
  return JSON.stringify({ ok: true, email: email });
};
