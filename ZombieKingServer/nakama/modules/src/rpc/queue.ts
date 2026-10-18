// 撮合流（架构文档 §4.4）：自研队列处理器为主路径
// - rpc_queue_enter/cancel/status：客户端 REST/WS 排队（UE 客户端亦可轮询 status 取 join_token）
// - zk_queueproc 常驻 Match：每秒 tick 扫描 match_queue，按 MMR 窗口(随等待放宽)成组 →
//   createMatchForGroup(分配 DS + 登记 + join_token + 创建 zk_match 广播)
// - matchmakerMatched：内置 matchmaker 回调（同样走共享分配逻辑，保留备用）

import { createMatchForGroup } from "../common/alloc";
import { parseInput, requireUuid } from "../common/api";
import { guardRate } from "../common/rateLimit";

const VALID_MODES: { [k: string]: boolean } = { tdm_5v5: true, zombie_infection: true };

function sysCfg(nk: nkruntime.Nakama, key: string, def: string): string {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : def;
}

// ---- 客户端 RPC ----

export const rpcQueueEnter: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_queue_enter", ctx.userId, "rate_asset_per_min", 30); }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES[mode]) { throw new Error("INVALID_INPUT: mode"); }

  const mmrRows = nk.sqlQuery("SELECT mmr FROM game.player_rating WHERE user_id=$1 AND mode=$2", [ctx.userId, mode]);
  const mmr = mmrRows.length ? parseInt(String(mmrRows[0].mmr), 10) : 1000;

  nk.sqlQuery(
    "INSERT INTO game.match_queue(user_id,mode,mmr,queued_at,match_id,join_token) VALUES ($1,$2,$3,now(),NULL,NULL) " +
    "ON CONFLICT (user_id) DO UPDATE SET mode=EXCLUDED.mode, mmr=EXCLUDED.mmr, queued_at=now(), match_id=NULL, join_token=NULL, party_id=NULL",
    [ctx.userId, mode, mmr]
  );

  ensureQueueProcessor(nk, logger);
  return JSON.stringify({ ok: true, mode: mode, mmr: mmr });
};

// 确保撮合处理器 Match 存在（懒创建，match_id 记在 sys_config；rpc_queue_enter 与 rpc_room_start 共用）
export function ensureQueueProcessor(nk: nkruntime.Nakama, logger: nkruntime.Logger): void {
  let procId = sysCfg(nk, "queue_proc_match_id", "");
  let alive = false;
  if (procId) {
    try { const m = (nk as any).matchGet(procId); alive = !!m; } catch (e) { alive = false; }
  }
  if (!alive) {
    procId = nk.matchCreate("zk_queueproc", {});
    nk.sqlQuery(
      "INSERT INTO game.sys_config(key,value,note) VALUES ('queue_proc_match_id',$1,'queue processor match') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value",
      [procId]
    );
    logger.info("queue processor created", "match", procId);
  }
}

export const rpcQueueCancel: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_queue_cancel", ctx.userId, "rate_asset_per_min", 30); }
  const del = nk.sqlQuery("DELETE FROM game.match_queue WHERE user_id=$1 AND match_id IS NULL RETURNING user_id", [ctx.userId]);
  return JSON.stringify({ ok: true, cancelled: del.length > 0 });
};

export const rpcQueueStatus: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_queue_status", ctx.userId, "rate_lobby_per_min", 120); }
  const rows = nk.sqlQuery(
    "SELECT match_id::text AS match_id, join_token FROM game.match_queue WHERE user_id=$1", [ctx.userId]);
  if (!rows.length) { return JSON.stringify({ queued: false }); }
  if (!rows[0].match_id) { return JSON.stringify({ queued: true, matched: false }); }
  return JSON.stringify({
    queued: true, matched: true,
    match_id: String(rows[0].match_id), join_token: String(rows[0].join_token),
  });
};

// ---- zk_queueproc：撮合处理器 ----

const zkQueueProcInit: nkruntime.MatchInitFunction<any> = function (ctx, logger, nk, params) {
  return { state: { tick: 0 }, tickRate: 1, label: "zk_queueproc" };
};
const zkQueueProcJoinAttempt: nkruntime.MatchJoinAttemptFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  return { state: state, accept: false };
};
const zkQueueProcJoin: nkruntime.MatchJoinFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presences) { return { state: state }; };
const zkQueueProcLeave: nkruntime.MatchLeaveFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presences) { return { state: state }; };
const zkQueueProcSignal: nkruntime.MatchSignalFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, data) { return { state: state, data: data }; };
const zkQueueProcTerminate: nkruntime.MatchTerminateFunction<any> = function (ctx, logger, nk, dispatcher, tick, graceSeconds) { return {}; };

const zkQueueProcLoop: nkruntime.MatchLoopFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, messages) {
  // R3 超时无效果局处理 + 限流窗口清理（每 10 tick 一次，避免每秒全表扫描）
  state.tick = (state.tick || 0) + 1;
  if (state.tick % 10 === 0) {
    const voided = nk.sqlQuery(
      "UPDATE game.match_record SET status='void' WHERE status='pending' AND expire_at < now() RETURNING match_id::text AS mid");
    for (let i = 0; i < voided.length; i++) {
      nk.sqlQuery("DELETE FROM game.match_queue WHERE match_id=$1::uuid", [String(voided[i].mid)]);
      logger.info("match voided (expire without settle)", "match", String(voided[i].mid));
    }
    nk.sqlQuery("DELETE FROM game.rate_limit WHERE window_start < now() - interval '5 minutes'");
  }

  const minTdm = parseInt(sysCfg(nk, "queue_min_tdm", "2"), 10);
  const minZom = parseInt(sysCfg(nk, "queue_min_zombie", "2"), 10);
  const winBase = parseInt(sysCfg(nk, "queue_mmr_window", "150"), 10);

  const rows = nk.sqlQuery(
    "SELECT user_id::text AS uid, mode, mmr, party_id::text AS party, extract(epoch from (now()-queued_at))::int AS waited " +
    "FROM game.match_queue WHERE match_id IS NULL ORDER BY mode, mmr");
  if (!rows.length) { return { state: state }; }

  const byMode: { [m: string]: any[] } = {};
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    if (!byMode[r.mode]) { byMode[r.mode] = []; }
    byMode[r.mode].push(r);
  }

  const modes = Object.keys(byMode);
  for (let mi = 0; mi < modes.length; mi++) {
    const mode = modes[mi];
    const need = mode === "tdm_5v5" ? minTdm : minZom;

    // 拆分：party（房间整队）与 solo
    const parties: { [pid: string]: any[] } = {};
    const solos: any[] = [];
    const arr = byMode[mode];
    for (let i = 0; i < arr.length; i++) {
      const r = arr[i];
      if (r.party) {
        if (!parties[r.party]) { parties[r.party] = []; }
        parties[r.party].push(r);
      } else {
        solos.push(r);
      }
    }

    // 1) party 优先成组：人数>=need 立即成组；不足则用 solo 补位
    const pids = Object.keys(parties);
    for (let pi = 0; pi < pids.length; pi++) {
      const grp = parties[pids[pi]];
      const ids: string[] = [];
      for (let k = 0; k < grp.length; k++) { ids.push(String(grp[k].uid)); }
      if (grp.length < need) {
        const lack = need - grp.length;
        if (solos.length < lack) { continue; } // 补不齐，继续等
        for (let k = 0; k < lack; k++) { ids.push(String(solos.shift().uid)); }
      }
      const mid = createMatchForGroup(nk, logger, mode, ids);
      if (!mid) { break; } // 分配失败（如 Manager 不可用），下一 tick 重试
    }

    // 2) 剩余 solo 按 MMR 窗口成组（窗口随等待放宽）
    let i = 0;
    while (i + need <= solos.length) {
      const j = i + need - 1;
      let maxWait = 0;
      for (let k = i; k <= j; k++) { if (solos[k].waited > maxWait) { maxWait = solos[k].waited; } }
      const win = winBase + 50 * Math.floor(maxWait / 10);
      if (Number(solos[j].mmr) - Number(solos[i].mmr) <= win) {
        const ids: string[] = [];
        for (let k = i; k < i + need; k++) { ids.push(String(solos[k].uid)); }
        const mid = createMatchForGroup(nk, logger, mode, ids);
        if (!mid) { break; }
        i += need;
      } else {
        i += 1;
      }
    }
  }
  return { state: state };
};

export const zkQueueProcHandlers: nkruntime.MatchHandler = {
  matchInit: zkQueueProcInit,
  matchJoinAttempt: zkQueueProcJoinAttempt,
  matchJoin: zkQueueProcJoin,
  matchLeave: zkQueueProcLeave,
  matchLoop: zkQueueProcLoop,
  matchTerminate: zkQueueProcTerminate,
  matchSignal: zkQueueProcSignal,
};

// ---- 内置 matchmaker 回调（保留备用）----

export const matchmakerMatched: nkruntime.MatchmakerMatchedFunction = function (ctx, logger, nk, entries) {
  if (!entries || entries.length < 2) { return null; }
  const mode = String((entries[0] as any).properties.mode || "");
  if (!VALID_MODES[mode]) { return null; }
  const ids: string[] = [];
  for (let i = 0; i < entries.length; i++) { ids.push(String(entries[i].presence.userId)); }
  const mid = createMatchForGroup(nk, logger, mode, ids);
  return mid ? mid : null;
};
