// P3 对局链路（架构文档 §4.5/§4.6）：
// - rpc_match_create：分配 DS（dev 本地分配，DS Manager 就绪后切换）、登记 match_record、
//   为每个玩家生成 join_token（loadout 装备快照 + HMAC(ds_secret)）。
// - rpc_match_settle：仅信任受信任 DS（红线 R3）——签名/上下文/时间窗/玩家集合/按模式统计
//   全部复验通过后，fn_match_settle 单事务入账。
// join_token 格式：payloadJson + "." + hex(hmacSha256(payloadJson, ds_secret))，DS 侧本地验签。

import { parseInput, callFn, failIfErr, decodeJsonb, requireUuid } from "../common/api";
import { hmacHex, verifySettleSig } from "../common/hmac";

const MODES: { [k: string]: boolean } = { tdm_5v5: true, zombie_infection: true };
const GRACE_SEC = 300;

function isDevMode(nk: nkruntime.Nakama): boolean {
  const cfg = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key='dev_mode'");
  return cfg.length > 0 && String(cfg[0].value) === "true";
}

function audit(nk: nkruntime.Nakama, actor: string, action: string, target: string, detailJson: string) {
  nk.sqlQuery(
    "INSERT INTO game.audit_log(actor,action,target,detail) VALUES ($1,$2,$3,$4::jsonb)",
    [actor, action, target, detailJson]
  );
}

export const rpcMatchCreate: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (!isDevMode(nk)) { throw new Error("DEV_ONLY_ENTRY"); }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!MODES[mode]) { throw new Error("INVALID_INPUT: mode"); }
  const players: any[] = input.players || [];
  if (players.length < 2 || players.length > 10) { throw new Error("INVALID_INPUT: players"); }
  const duration = Math.min(3600, Math.max(120, parseInt(String(input.duration_sec || 900), 10)));

  const matchId = nk.uuidv4();
  const dsId = nk.uuidv4();
  const dsSecret = nk.uuidv4().replace(/-/g, "") + nk.uuidv4().replace(/-/g, "");
  const nowSec = Math.floor(Date.now() / 1000);
  const expireAt = nowSec + duration + GRACE_SEC;

  const set: any[] = [];
  for (let i = 0; i < players.length; i++) {
    set.push({ user_id: requireUuid(players[i].user_id, "players[" + i + "].user_id"), team: parseInt(String(players[i].team), 10) });
  }

  // dev 本地分配（DS Manager 就绪后改为 HTTP 调用 /v1/allocations）
  nk.sqlQuery(
    "INSERT INTO game.match_record(match_id,mode,map,ds_id,ds_host_id,player_set,started_at,expire_at,status,ds_secret) " +
    "VALUES ($1,$2,$3,$4,'dev-host',$5::jsonb,to_timestamp($6),to_timestamp($7),'pending',$8)",
    [matchId, mode, String(input.map || "dm_simple"), dsId, JSON.stringify(set), nowSec, expireAt, dsSecret]
  );

  const tokens: any[] = [];
  for (let i = 0; i < set.length; i++) {
    const lo = nk.sqlQuery("SELECT slots::text AS slots FROM game.loadout WHERE user_id=$1 AND mode=$2", [set[i].user_id, mode]);
    const snapshot = lo.length ? decodeJsonb(lo[0].slots) : {};
    const payload = JSON.stringify({
      match_id: matchId, user_id: set[i].user_id, team: set[i].team,
      loadout: snapshot, issued_at: nowSec, expire_at: expireAt,
    });
    tokens.push({ user_id: set[i].user_id, join_token: payload + "." + hmacHex(nk, payload, dsSecret) });
  }

  logger.info("match_created", "match", matchId, "mode", mode, "players", set.length);
  return JSON.stringify({
    match_id: matchId, ds_id: dsId, ds_ip: "127.0.0.1", ds_port: 7777,
    mode: mode, expire_at: expireAt, tokens: tokens,
  });
};

export const rpcMatchSettle: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  const p = parseInput(params);
  const matchId = requireUuid(p.match_id, "match_id");

  const recs = nk.sqlQuery(
    "SELECT status, ds_id::text AS ds_id, ds_secret, mode, player_set::text AS player_set, " +
    "extract(epoch from started_at)::bigint AS started_epoch, extract(epoch from expire_at)::bigint AS expire_epoch " +
    "FROM game.match_record WHERE match_id=$1",
    [matchId]
  );
  if (!recs.length) { throw new Error("MATCH_NOT_FOUND"); }
  const rec = recs[0];
  if (String(rec.status) !== "pending") {
    return JSON.stringify({ ok: true, replayed: true });
  }
  const dsSecret = rec.ds_secret ? String(rec.ds_secret) : "";
  if (!dsSecret) { throw new Error("SECRET_CLEARED"); }

  const fail = function (code: string) {
    audit(nk, "ds", "settle_rejected", matchId, JSON.stringify({ code: code, ds_id: String(p.ds_id || "") }));
    nk.sqlQuery("UPDATE game.ds_host hs SET suspicious_count=suspicious_count+1 FROM game.match_record mr WHERE mr.match_id=$1 AND hs.host_id=mr.ds_host_id", [matchId]);
    throw new Error(code);
  };

  // 1. HMAC 签名复验（canonical = match_id|ds_id|settled_at|payload_hash）
  if (!verifySettleSig(nk, p, dsSecret)) { fail("SETTLE_SIG_INVALID"); }
  // 2. ds_id 吻合
  if (String(p.ds_id) !== String(rec.ds_id)) { fail("SETTLE_DS_MISMATCH"); }
  // 3. 时间窗
  const settledAt = Number(p.settled_at);
  if (!(settledAt >= Number(rec.started_epoch) - 60 && settledAt <= Number(rec.expire_epoch))) {
    fail("SETTLE_TIME_WINDOW");
  }
  // 4. 玩家集合完全一致（不多不少、team 相符）
  const expected = decodeJsonb(rec.player_set) as any[];
  const got: any[] = p.players || [];
  if (got.length !== expected.length) { fail("SETTLE_PLAYER_SET"); }
  const expMap: { [k: string]: number } = {};
  for (let i = 0; i < expected.length; i++) { expMap[String(expected[i].user_id)] = parseInt(String(expected[i].team), 10); }
  const gotSet: { [k: string]: boolean } = {};
  for (let i = 0; i < got.length; i++) {
    const uid = String(got[i].user_id);
    if (expMap[uid] === undefined || expMap[uid] !== parseInt(String(got[i].team), 10) || gotSet[uid]) {
      fail("SETTLE_PLAYER_SET");
    }
    gotSet[uid] = true;
  }

  // 5. 按模式 stats 合法性
  const mode = String(rec.mode);
  const results: any[] = [];
  const rewards: any[] = [];
  const rating: any[] = [];
  const tdmKills: { [k: string]: number } = {};
  const tdmDeaths: { [k: string]: number } = {};
  const winnerTeam = p.outcome ? parseInt(String(p.outcome.winner_team), 10) : -1;
  const winnerFaction = p.outcome ? String(p.outcome.winner_faction || "") : "";

  for (let i = 0; i < got.length; i++) {
    const pl = got[i];
    const st = pl.stats || {};
    const kills = Math.max(0, parseInt(String(st.kills || 0), 10));
    const deaths = Math.max(0, parseInt(String(st.deaths || 0), 10));
    if (kills > 200 || deaths > 200) { fail("SETTLE_STATS_RANGE"); }

    let won = false;
    if (mode === "tdm_5v5") {
      const team = parseInt(String(pl.team), 10);
      tdmKills[team] = (tdmKills[team] || 0) + kills;
      tdmDeaths[team] = (tdmDeaths[team] || 0) + deaths;
      won = team === winnerTeam;
    } else {
      const role = String(st.role || "");
      if (role !== "human" && role !== "zombie") { fail("SETTLE_ROLE_INVALID"); }
      won = role === winnerFaction;
    }

    results.push({ user_id: String(pl.user_id), team: parseInt(String(pl.team), 10), stats: st });
    const gold = 200 + kills * 20 + (won ? 500 : 0);
    rewards.push({ user_id: String(pl.user_id), currency: "gold", amount: gold });
    rating.push({ user_id: String(pl.user_id), mode: mode, delta: won ? 25 : -25, won: won });
  }

  if (mode === "tdm_5v5") {
    const teams = Object.keys(tdmKills);
    if (teams.length !== 2) { fail("SETTLE_TEAMS"); }
    const t1 = teams[0]; const t2 = teams[1];
    if (tdmDeaths[t1] !== tdmKills[t2] || tdmDeaths[t2] !== tdmKills[t1]) { fail("SETTLE_CROSSCHECK"); }
  } else if (mode === "zombie_infection") {
    if (winnerFaction !== "human" && winnerFaction !== "zombie") { fail("SETTLE_OUTCOME_INVALID"); }
    // 感染状态转移深度校验（§4.6.2）：事件流按时间单调升序；僵尸集合与事件集合一一对应；t_sec 非负
    const events: any[] = (p.outcome && p.outcome.infection_events) || [];
    for (let i = 1; i < events.length; i++) {
      if (Number(events[i].t_sec) < Number(events[i - 1].t_sec)) { fail("SETTLE_EVENTS_ORDER"); }
    }
    const zombieSet: { [k: string]: boolean } = {};
    for (let i = 0; i < got.length; i++) {
      if (String((got[i].stats || {}).role) === "zombie") { zombieSet[String(got[i].user_id)] = true; }
    }
    const eventSet: { [k: string]: boolean } = {};
    for (let i = 0; i < events.length; i++) {
      const uid = String(events[i].user_id);
      if (!zombieSet[uid] || Number(events[i].t_sec) < 0 || eventSet[uid]) { fail("SETTLE_EVENTS_MISMATCH"); }
      eventSet[uid] = true;
    }
    if (Object.keys(eventSet).length !== Object.keys(zombieSet).length) { fail("SETTLE_EVENTS_MISMATCH"); }
  }

  const res = failIfErr(callFn(nk, "fn_match_settle", [
    matchId, JSON.stringify(results), JSON.stringify(rewards), JSON.stringify(rating), String(p.payload_hash),
  ]));
  logger.info("match_settled", "match", matchId, "players", got.length);
  return JSON.stringify(res);
};
