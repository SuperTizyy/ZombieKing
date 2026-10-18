// 撮合成组 → DS 分配 → match_record 登记 → join_token 生成 → zk_match 创建
// 供 matchmakerMatched（内置撮合回调）与 zk_queueproc（自研队列处理器）共用。

import { rollFloat } from "./api";
import { hmacHex } from "./hmac";

const VALID_MODES: { [k: string]: boolean } = { tdm_5v5: true, zombie_infection: true };
const GRACE_SEC = 300;

function sysConfig(nk: nkruntime.Nakama, key: string): string {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : "";
}

// userIds: string[]。返回 match_id（失败返回 ""）。
export function createMatchForGroup(nk: nkruntime.Nakama, logger: nkruntime.Logger, mode: string, userIds: string[]): string {
  if (!VALID_MODES[mode] || userIds.length < 2) { return ""; }

  const managerUrl = sysConfig(nk, "ds_manager_url");
  const serviceToken = sysConfig(nk, "ds_service_token");
  if (!managerUrl || !serviceToken) {
    logger.warn("alloc: ds_manager not configured");
    return "";
  }

  const half = Math.ceil(userIds.length / 2);
  const flip = rollFloat(nk) < 0.5 ? 1 : 0;
  const players: any[] = [];
  for (let i = 0; i < userIds.length; i++) {
    players.push({ user_id: userIds[i], team: i < half ? 1 + flip : 2 - flip });
  }

  const matchId = nk.uuidv4();
  const duration = 900;
  const allocBody = JSON.stringify({
    match_id: matchId, mode: mode, map: mode === "tdm_5v5" ? "dm_simple" : "zc_infection",
    duration_sec: duration, players: players,
  });

  let resp: any;
  try {
    resp = (nk as any).httpRequest(managerUrl + "/v1/allocations", "post",
      { "Content-Type": "application/json", "X-Service-Token": serviceToken }, allocBody, 4000);
  } catch (e) {
    logger.warn("alloc: ds manager unreachable", String(e));
    return "";
  }
  if (resp.code !== 200) {
    logger.warn("alloc: failed code=" + resp.code + " body=" + resp.body);
    return "";
  }
  const alloc = JSON.parse(resp.body);

  const nowSec = Math.floor(Date.now() / 1000);
  const expireAt = nowSec + duration + GRACE_SEC;
  nk.sqlQuery(
    "INSERT INTO game.match_record(match_id,mode,map,ds_id,ds_host_id,player_set,started_at,expire_at,status,ds_secret) " +
    "VALUES ($1,$2,$3,$4,$5,$6::jsonb,to_timestamp($7),to_timestamp($8),'pending',$9)",
    [matchId, mode, alloc.map || "", alloc.ds_id, alloc.ds_host_id, JSON.stringify(players), nowSec, expireAt, alloc.ds_secret]
  );

  const tokens: any[] = [];
  for (let i = 0; i < players.length; i++) {
    const lo = nk.sqlQuery("SELECT slots::text AS slots FROM game.loadout WHERE user_id=$1 AND mode=$2", [players[i].user_id, mode]);
    let snapshot: any = {};
    try { snapshot = lo.length ? JSON.parse(String(lo[0].slots)) : {}; } catch (e) { snapshot = {}; }
    const payload = JSON.stringify({
      match_id: matchId, user_id: players[i].user_id, team: players[i].team,
      loadout: snapshot, issued_at: nowSec, expire_at: expireAt,
    });
    const token = payload + "." + hmacHex(nk, payload, alloc.ds_secret);
    tokens.push({ user_id: players[i].user_id, join_token: token });
    // 写回队列行供 REST 轮询取用（WS 客户端走 zk_match 广播）
    nk.sqlQuery("UPDATE game.match_queue SET match_id=$1, join_token=$2 WHERE user_id=$3", [matchId, token, players[i].user_id]);
  }

  logger.info("alloc ok", "match", matchId, "mode", mode, "players", players.length, "ds", alloc.ds_id);
  nk.matchCreate("zk_match", {
    match_id: matchId, ds_ip: alloc.ds_ip, ds_port: alloc.ds_port,
    mode: mode, expire_at: expireAt, tokens: tokens,
  });
  return matchId;
}

// ---- zk_match：向已撮合玩家广播 join_token（WS 路径；REST 客户端走 rpc_queue_status）----

const zkMatchInit: nkruntime.MatchInitFunction<any> = function (ctx, logger, nk, params) {
  return { state: { meta: params, broadcasted: false }, tickRate: 1, label: "zk_match" };
};
const zkMatchJoinAttempt: nkruntime.MatchJoinAttemptFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  return { state: state, accept: true };
};
const zkMatchJoin: nkruntime.MatchJoinFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state: state };
};
const zkMatchLeave: nkruntime.MatchLeaveFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state: state };
};
const zkMatchLoop: nkruntime.MatchLoopFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, messages) {
  if (!state.broadcasted) {
    dispatcher.broadcastMessage(100, JSON.stringify(state.meta), null, null, true);
    state.broadcasted = true;
  }
  return { state: state };
};
const zkMatchTerminate: nkruntime.MatchTerminateFunction<any> = function (ctx, logger, nk, dispatcher, tick, graceSeconds) {
  return {};
};
const zkMatchSignal: nkruntime.MatchSignalFunction<any> = function (ctx, logger, nk, dispatcher, tick, state, data) {
  return { state: state, data: data };
};

export const zkMatchHandlers: nkruntime.MatchHandler = {
  matchInit: zkMatchInit,
  matchJoinAttempt: zkMatchJoinAttempt,
  matchJoin: zkMatchJoin,
  matchLeave: zkMatchLeave,
  matchLoop: zkMatchLoop,
  matchTerminate: zkMatchTerminate,
  matchSignal: zkMatchSignal,
};
