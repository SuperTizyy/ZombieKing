// 房间组队（F4）：game.room/room_member 权威成员表 + zk_lobby Match 承载房间聊天。
// 开局：房主 rpc_room_start → 全员写 match_queue（party_id=room_id）→ zk_queueproc 整队成组。

import { parseInput, requireUuid, bytesToStr } from "../common/api";
import { guardRate } from "../common/rateLimit";
import { ensureQueueProcessor } from "./queue";

const VALID_MODES: { [k: string]: boolean } = { tdm_5v5: true, zombie_infection: true };
const MAX_MEMBERS_CAP = 10;

function currentRoomId(nk: nkruntime.Nakama, userId: string): string {
  const rows = nk.sqlQuery("SELECT room_id::text AS rid FROM game.room_member WHERE user_id=$1::uuid", [userId]);
  return rows.length ? String(rows[0].rid) : "";
}

export const rpcRoomCreate: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_room_create", ctx.userId, "rate_asset_per_min", 30); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES[mode]) { throw new Error("INVALID_INPUT: mode"); }
  if (currentRoomId(nk, ctx.userId)) { throw new Error("IN_ROOM: 已在房间中，请先离开"); }

  let max = parseInt(String(input.max_members || MAX_MEMBERS_CAP), 10);
  if (isNaN(max) || max < 2) { max = 2; }
  if (max > MAX_MEMBERS_CAP) { max = MAX_MEMBERS_CAP; }
  const map = String(input.map || "").substring(0, 64);

  const roomId = nk.uuidv4();
  nk.sqlQuery(
    "INSERT INTO game.room(room_id,owner_id,mode,map,max_members) VALUES ($1::uuid,$2::uuid,$3,$4,$5)",
    [roomId, ctx.userId, mode, map, max]);
  nk.sqlQuery("INSERT INTO game.room_member(room_id,user_id) VALUES ($1::uuid,$2::uuid)", [roomId, ctx.userId]);

  // v3.41 实测 matchCreate 返回 "uuid.<node>" 格式，match_id 列为 text
  const matchId = nk.matchCreate("zk_lobby", { room_id: roomId });
  nk.sqlQuery("UPDATE game.room SET match_id=$1 WHERE room_id=$2::uuid", [matchId, roomId]);
  logger.info("room created", "room", roomId, "owner", ctx.userId, "match", matchId);
  return JSON.stringify({ ok: true, room_id: roomId, match_id: matchId, mode: mode, map: map, max_members: max });
};

export const rpcRoomJoin: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_room_join", ctx.userId, "rate_asset_per_min", 30); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  const input = parseInput(params);
  const roomId = requireUuid(input.room_id, "room_id");

  const mine = currentRoomId(nk, ctx.userId);
  if (mine) {
    if (mine === roomId) {
      const r = nk.sqlQuery("SELECT match_id AS mid FROM game.room WHERE room_id=$1::uuid", [roomId]);
      return JSON.stringify({ ok: true, room_id: roomId, match_id: r.length ? String(r[0].mid) : "" }); // 幂等重放
    }
    throw new Error("IN_OTHER_ROOM: 已在其他房间");
  }

  const rows = nk.sqlQuery(
    "SELECT mode, map, max_members, match_id AS mid, " +
    "(SELECT count(*) FROM game.room_member m WHERE m.room_id=r.room_id)::int AS members " +
    "FROM game.room r WHERE room_id=$1::uuid", [roomId]);
  if (!rows.length) { throw new Error("ROOM_NOT_FOUND"); }
  const room = rows[0];
  if (parseInt(String(room.members), 10) >= parseInt(String(room.max_members), 10)) { throw new Error("ROOM_FULL"); }

  nk.sqlQuery("INSERT INTO game.room_member(room_id,user_id) VALUES ($1::uuid,$2::uuid) ON CONFLICT DO NOTHING", [roomId, ctx.userId]);
  return JSON.stringify({
    ok: true, room_id: roomId, match_id: String(room.mid),
    mode: String(room.mode), map: String(room.map), members: parseInt(String(room.members), 10) + 1,
  });
};

export const rpcRoomLeave: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_room_leave", ctx.userId, "rate_asset_per_min", 30); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params);
  const roomId = currentRoomId(nk, ctx.userId);
  if (!roomId) { return JSON.stringify({ ok: true, left: false }); }

  const owner = nk.sqlQuery("SELECT owner_id::text AS oid FROM game.room WHERE room_id=$1::uuid", [roomId]);
  if (owner.length && String(owner[0].oid) === ctx.userId) {
    nk.sqlQuery("DELETE FROM game.room WHERE room_id=$1::uuid", [roomId]); // cascade 清成员
    logger.info("room dissolved by owner", "room", roomId);
    return JSON.stringify({ ok: true, left: true, dissolved: true });
  }
  nk.sqlQuery("DELETE FROM game.room_member WHERE room_id=$1::uuid AND user_id=$2::uuid", [roomId, ctx.userId]);
  return JSON.stringify({ ok: true, left: true, dissolved: false });
};

export const rpcRoomList: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_room_list", ctx.userId, "rate_lobby_per_min", 120); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params);
  const rows = nk.sqlQuery(
    "SELECT r.room_id::text AS rid, r.mode, r.map, r.max_members, u.username AS owner, " +
    "(SELECT count(*) FROM game.room_member m WHERE m.room_id=r.room_id)::int AS members " +
    "FROM game.room r JOIN users u ON u.id=r.owner_id ORDER BY r.created_at DESC LIMIT 50");
  const rooms: any[] = [];
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    rooms.push({
      room_id: String(r.rid), mode: String(r.mode), map: String(r.map),
      owner: String(r.owner), members: parseInt(String(r.members), 10),
      max_members: parseInt(String(r.max_members), 10),
    });
  }
  return JSON.stringify({ ok: true, rooms: rooms });
};

export const rpcRoomStart: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_room_start", ctx.userId, "rate_asset_per_min", 30); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params);
  const roomId = currentRoomId(nk, ctx.userId);
  if (!roomId) { throw new Error("NOT_IN_ROOM"); }

  const room = nk.sqlQuery("SELECT owner_id::text AS oid, mode FROM game.room WHERE room_id=$1::uuid", [roomId]);
  if (!room.length) { throw new Error("ROOM_NOT_FOUND"); }
  if (String(room[0].oid) !== ctx.userId) { throw new Error("NOT_OWNER: 仅房主可开局"); }
  const mode = String(room[0].mode);

  const members = nk.sqlQuery(
    "SELECT user_id::text AS uid FROM game.room_member WHERE room_id=$1::uuid", [roomId]);
  if (members.length < 2) { throw new Error("NOT_ENOUGH_PLAYERS: 至少 2 人"); }

  for (let i = 0; i < members.length; i++) {
    const uid = String(members[i].uid);
    const mmrRows = nk.sqlQuery("SELECT mmr FROM game.player_rating WHERE user_id=$1::uuid AND mode=$2", [uid, mode]);
    const mmr = mmrRows.length ? parseInt(String(mmrRows[0].mmr), 10) : 1000;
    nk.sqlQuery(
      "INSERT INTO game.match_queue(user_id,mode,mmr,queued_at,match_id,join_token,party_id) " +
      "VALUES ($1::uuid,$2,$3,now(),NULL,NULL,$4::uuid) " +
      "ON CONFLICT (user_id) DO UPDATE SET mode=EXCLUDED.mode, mmr=EXCLUDED.mmr, queued_at=now(), " +
      "match_id=NULL, join_token=NULL, party_id=EXCLUDED.party_id",
      [uid, mode, mmr, roomId]);
  }
  nk.sqlQuery("DELETE FROM game.room WHERE room_id=$1::uuid", [roomId]);
  ensureQueueProcessor(nk, logger); // 房间开局绕过 rpc_queue_enter，需自行确保处理器存活
  logger.info("room started", "room", roomId, "players", members.length, "mode", mode);
  return JSON.stringify({ ok: true, started: members.length, mode: mode });
};

// ---- zk_lobby：房间聊天 Match。op=1 房间聊天广播；鉴权=room_member 表；房间删除/空房超时即终止 ----

interface LobbyState { room_id: string; presences: any[]; idle: number; }

const zkLobbyInit: nkruntime.MatchInitFunction<LobbyState> = function (ctx, logger, nk, params) {
  return { state: { room_id: String((params as any).room_id || ""), presences: [], idle: 0 }, tickRate: 1, label: "zk_lobby" };
};

const zkLobbyJoinAttempt: nkruntime.MatchJoinAttemptFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  if (!state.room_id) { return { state: state, accept: false }; }
  const rows = nk.sqlQuery(
    "SELECT 1 AS ok FROM game.room_member WHERE room_id=$1::uuid AND user_id=$2::uuid", [state.room_id, presence.userId]);
  return { state: state, accept: rows.length > 0 };
};

const zkLobbyJoin: nkruntime.MatchJoinFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, state, presences) {
  for (let i = 0; i < presences.length; i++) { state.presences.push(presences[i]); }
  state.idle = 0;
  return { state: state };
};

const zkLobbyLeave: nkruntime.MatchLeaveFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, state, presences) {
  for (let i = 0; i < presences.length; i++) {
    const p = presences[i];
    for (let j = state.presences.length - 1; j >= 0; j--) {
      if (state.presences[j].sessionId === p.sessionId) { state.presences.splice(j, 1); }
    }
  }
  return { state: state };
};

const zkLobbyLoop: nkruntime.MatchLoopFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, state, messages) {
  // 房间已被删除（开局/房主离开）→ 终止 Match
  const exists = nk.sqlQuery("SELECT 1 AS ok FROM game.room WHERE room_id=$1::uuid", [state.room_id]);
  if (!exists.length) { return null; }

  // v3.41 实测 matchLoop 消息字段为 opCode（非 op），data 为字节数组
  for (let i = 0; i < messages.length; i++) {
    const m = messages[i];
    const op = Number((m as any).opCode);
    if (op >= 1 && op <= 9) {
      // v3.41 实测：传 m.sender 报 "expects sender to be an object"，广播全员（含发送者，客户端按 user_id 去重）
      // 实测签名 broadcastMessage(op, data, presences, sender, reliable)——sender 第 4、reliable 第 5
      dispatcher.broadcastMessage(op, bytesToStr(m.data), null, null, true);
    }
  }
  if (state.presences.length === 0) {
    state.idle += 1;
    if (state.idle > 10) { return null; } // 空房 10s 回收
  } else {
    state.idle = 0;
  }
  return { state: state };
};

const zkLobbyTerminate: nkruntime.MatchTerminateFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, graceSeconds) {
  return { room_id: "", presences: [], idle: 0 };
};

const zkLobbySignal: nkruntime.MatchSignalFunction<LobbyState> = function (ctx, logger, nk, dispatcher, tick, state, data) {
  return { state: state, data: data };
};

export const zkLobbyHandlers: nkruntime.MatchHandler = {
  matchInit: zkLobbyInit,
  matchJoinAttempt: zkLobbyJoinAttempt,
  matchJoin: zkLobbyJoin,
  matchLeave: zkLobbyLeave,
  matchLoop: zkLobbyLoop,
  matchTerminate: zkLobbyTerminate,
  matchSignal: zkLobbySignal,
};
