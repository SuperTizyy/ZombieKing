// 登录后一次性拉取：钱包余额、背包实例、各模式 loadout（只读，读 game.*）
// jsonb 列在 goja 中是字节数组，统一 decodeJsonb 还原为对象再返回给客户端

import { decodeJsonb } from "../common/api";

export const rpcBootstrapGet: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  const wallet = nk.sqlQuery(
    "SELECT currency, balance FROM game.wallet WHERE user_id=$1 ORDER BY currency",
    [ctx.userId]
  );
  const inventory = nk.sqlQuery(
    "SELECT id, item_def_id, quality, level, status FROM game.inventory " +
    "WHERE user_id=$1 AND status <> 'consumed' ORDER BY id",
    [ctx.userId]
  );
  const loadouts = nk.sqlQuery(
    "SELECT mode, slots::text AS slots, version FROM game.loadout WHERE user_id=$1 ORDER BY mode",
    [ctx.userId]
  );
  for (let i = 0; i < loadouts.length; i++) {
    loadouts[i].slots = decodeJsonb(loadouts[i].slots);
  }
  return JSON.stringify({ wallet: wallet, inventory: inventory, loadouts: loadouts });
};
