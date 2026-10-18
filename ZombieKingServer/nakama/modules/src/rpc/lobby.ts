// 大厅世界聊天频道（F2）：Nakama 无全局频道，用公开 Group 承载，客户端 ChatChannelJoin(type=2)。
// Group 成员上限 100（Nakama 硬限制），因此按分片扩展：zk-lobby-1..N，
// rpc_lobby_channel_get 返回首个有空位的分片；全满则懒建新分片。group_id 记于 game.sys_config。
// 注意：v3.41 实测 RPC 会话下 nk.groupCreate 报 "expects user ID to be a valid identifier"
//（绑定实现取不到 session 用户），改由服务端 httpRequest 调本机 REST，以专用 lobby-bot 设备账号建组。

import { parseInput } from "../common/api";
import { guardRate } from "../common/rateLimit";

const SHARD_PREFIX = "zk-lobby-";
const SHARD_MAX = 100;
const LOBBY_BOT_DEVICE_ID = "zk-lobby-bot";

const B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64ascii(s: string): string {
  let out = "";
  let i = 0;
  while (i < s.length) {
    const c0 = s.charCodeAt(i++);
    const c1 = i < s.length ? s.charCodeAt(i++) : -1;
    const c2 = i < s.length ? s.charCodeAt(i++) : -1;
    out += B64.charAt(c0 >> 2);
    if (c1 === -1) { out += B64.charAt((c0 & 3) << 4) + "=="; break; }
    out += B64.charAt(((c0 & 3) << 4) | (c1 >> 4));
    if (c2 === -1) { out += B64.charAt((c1 & 15) << 2) + "="; break; }
    out += B64.charAt(((c1 & 15) << 2) | (c2 >> 6));
    out += B64.charAt(c2 & 63);
  }
  return out;
}

// lobby-bot 设备账号登录取 token（建组/加成员用，bot 为群 creator=superadmin）
function lobbyBotToken(nk: nkruntime.Nakama): string {
  const selfUrl = sysCfg(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const serverKey = sysCfg(nk, "socket_server_key", "");
  if (!serverKey) { throw new Error("NOT_CONFIGURED: sys_config 缺少 socket_server_key"); }
  const login = nk.httpRequest(selfUrl + "/v2/account/authenticate/device?create=true", "post",
    { "Authorization": "Basic " + b64ascii(serverKey + ":"), "Content-Type": "application/json" },
    JSON.stringify({ id: LOBBY_BOT_DEVICE_ID }), 3000);
  if (login.code !== 200) { throw new Error("LOBBY_BOT_LOGIN_FAILED: code=" + login.code); }
  return String(JSON.parse(login.body).token);
}

// 以 lobby-bot 身份创建公开群组，返回 group_id
function createLobbyGroup(nk: nkruntime.Nakama, logger: nkruntime.Logger, name: string): string {
  const selfUrl = sysCfg(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const create = nk.httpRequest(selfUrl + "/v2/group", "post",
    { "Authorization": "Bearer " + lobbyBotToken(nk), "Content-Type": "application/json" },
    JSON.stringify({ name: name, open: true, lang_tag: "en", editable: false, max_count: SHARD_MAX, description: "lobby world channel" }), 3000);
  if (create.code !== 200) { throw new Error("LOBBY_GROUP_CREATE_FAILED: code=" + create.code + " body=" + create.body); }
  return String(JSON.parse(create.body).id);
}

// 把用户加入群组（WS channel_join 要求群成员权限>=2，否则报 Group not found）
function addLobbyGroupUser(nk: nkruntime.Nakama, groupId: string, userId: string): void {
  const selfUrl = sysCfg(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const resp = nk.httpRequest(selfUrl + "/v2/group/" + groupId + "/add?user_ids=" + userId, "post",
    { "Authorization": "Bearer " + lobbyBotToken(nk) }, "", 3000);
  if (resp.code !== 200 && resp.code !== 204) {
    throw new Error("LOBBY_GROUP_ADD_FAILED: code=" + resp.code + " body=" + resp.body);
  }
}

function sysCfg(nk: nkruntime.Nakama, key: string, def: string): string {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : def;
}

function sysCfgSet(nk: nkruntime.Nakama, key: string, value: string): void {
  nk.sqlQuery(
    "INSERT INTO game.sys_config(key,value,note) VALUES ($1,$2,'lobby channel') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value",
    [key, value]
  );
}

export const rpcLobbyChannelGet: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_lobby_channel_get", ctx.userId, "rate_lobby_per_min", 120); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params); // 允许空参

  const count = parseInt(sysCfg(nk, "lobby_channel_count", "0"), 10);
  // 找有空位的分片（Nakama 成员表为 group_edge：source_id=组、destination_id=用户；state<=2 已加入，3=申请中不计）
  let gid = "";
  let shard = 0;
  for (let i = 1; i <= count; i++) {
    const cand = sysCfg(nk, "lobby_group_" + i, "");
    if (!cand) { continue; }
    const rows = nk.sqlQuery(
      "SELECT count(*)::int AS cnt FROM group_edge WHERE source_id=$1::uuid AND state<=2", [cand]);
    const cnt = rows.length ? parseInt(String(rows[0].cnt), 10) : 0;
    if (cnt < SHARD_MAX) { gid = cand; shard = i; break; }
  }

  if (!gid) {
    // 全满/无分片 → 以 lobby-bot 身份新建分片
    shard = count + 1;
    gid = createLobbyGroup(nk, logger, SHARD_PREFIX + shard);
    sysCfgSet(nk, "lobby_group_" + shard, gid);
    sysCfgSet(nk, "lobby_channel_count", String(shard));
    logger.info("lobby shard created", "shard", shard, "group", gid);
  }

  // 确保调用者是群成员（WS channel_join 要求成员权限，服务端自动拉入）
  const member = nk.sqlQuery(
    "SELECT 1 AS ok FROM group_edge WHERE source_id=$1::uuid AND destination_id=$2::uuid AND state<=2",
    [gid, ctx.userId]);
  if (!member.length) {
    addLobbyGroupUser(nk, gid, ctx.userId);
  }
  return JSON.stringify({ ok: true, group_id: gid, shard: shard });
};

// ---- F3 在线列表：心跳 upsert + 60s 窗口查询 ----

const ONLINE_TTL_SEC = 60;

export const rpcLobbyHeartbeat: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_lobby_heartbeat", ctx.userId, "rate_lobby_per_min", 120); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params);

  let uname = ctx.username ? String(ctx.username) : "";
  if (!uname) {
    const rows = nk.sqlQuery("SELECT username FROM users WHERE id=$1", [ctx.userId]);
    uname = rows.length ? String(rows[0].username) : "";
  }
  nk.sqlQuery(
    "INSERT INTO game.online_users(user_id,username,last_seen) VALUES ($1::uuid,$2,now()) " +
    "ON CONFLICT (user_id) DO UPDATE SET last_seen=now(), username=EXCLUDED.username",
    [ctx.userId, uname]);
  return JSON.stringify({ ok: true });
};

export const rpcOnlineList: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  if (ctx.userId) { guardRate(nk, "rpc_online_list", ctx.userId, "rate_lobby_per_min", 120); }
  if (!ctx.userId) { throw new Error("UNAUTHENTICATED"); }
  parseInput(params);

  nk.sqlQuery("DELETE FROM game.online_users WHERE last_seen < now() - interval '" + ONLINE_TTL_SEC + " seconds'");
  const rows = nk.sqlQuery(
    "SELECT user_id::text AS uid, username FROM game.online_users WHERE user_id <> $1::uuid ORDER BY username LIMIT 200",
    [ctx.userId]);
  const online: any[] = [];
  for (let i = 0; i < rows.length; i++) {
    online.push({ user_id: String(rows[i].uid), username: String(rows[i].username) });
  }
  return JSON.stringify({ ok: true, online: online, ttl_sec: ONLINE_TTL_SEC });
};
