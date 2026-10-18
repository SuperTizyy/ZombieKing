// 换装（红线 R2）：TS 校验实例归属/槽位匹配/模式兼容 → fn_loadout_set 原子流转状态

import { parseInput, callFn, failIfErr } from "../common/api";
import { guardRate } from "../common/rateLimit";

const VALID_MODES: { [k: string]: boolean } = { tdm_5v5: true, zombie_infection: true };

export const rpcLoadoutSet: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_loadout_set", ctx.userId, "rate_asset_per_min", 30); }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES[mode]) { throw new Error("INVALID_INPUT: mode"); }

  const slots = input.slots || {};
  const keys = Object.keys(slots);
  if (keys.length === 0 || keys.length > 8) { throw new Error("INVALID_INPUT: slots"); }

  for (let i = 0; i < keys.length; i++) {
    const slotName = keys[i];
    const invId = parseInt(String(slots[slotName]), 10);
    if (!(invId > 0)) { throw new Error("INVALID_INPUT: slots." + slotName); }
    const ok = nk.sqlQuery(
      "SELECT 1 AS x FROM game.inventory i JOIN game.item_def d ON d.id=i.item_def_id " +
      "WHERE i.id=$1 AND i.user_id=$2 AND i.status IN ('in_bag','equipped') " +
      "AND d.slot=$3 AND $4 = ANY(d.mode_tags) AND d.enabled",
      [invId, ctx.userId, slotName, mode]
    );
    if (!ok.length) { throw new Error("SLOT_INCOMPATIBLE: " + slotName); }
  }

  const res = failIfErr(callFn(nk, "fn_loadout_set", [ctx.userId, mode, JSON.stringify(slots)]));
  logger.info("loadout_set", "user", ctx.userId, "mode", mode);
  return JSON.stringify(res);
};
