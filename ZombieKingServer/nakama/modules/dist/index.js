// src/common/storageGuard.ts
var SETTINGS_COLLECTION = "settings";
var SETTINGS_MAX_BYTES = 8192;
var beforeWriteStorageObjects = function(ctx, logger, nk, input) {
  const objects = input && input.objects ? input.objects : [];
  for (let i = 0; i < objects.length; i++) {
    const o = objects[i];
    if (o.collection !== SETTINGS_COLLECTION) {
      logger.warn("\u62D2\u7EDD\u5BA2\u6237\u7AEF\u5199 Storage", "userId", ctx.userId, "collection", o.collection);
      throw new Error("\u5BA2\u6237\u7AEF\u7981\u6B62\u5199\u5165\u8BE5\u96C6\u5408: " + o.collection);
    }
    const raw = typeof o.value === "string" ? o.value : JSON.stringify(o.value || {});
    if (raw.length > SETTINGS_MAX_BYTES) {
      logger.warn("\u62D2\u7EDD\u5BA2\u6237\u7AEF\u5199 settings \u8D85\u9650", "userId", ctx.userId, "bytes", raw.length);
      throw new Error("settings \u8D85\u51FA\u5927\u5C0F\u9650\u5236: " + raw.length + " bytes");
    }
  }
  return input;
};
var beforeDeleteStorageObjects = function(ctx, logger, nk, input) {
  logger.warn("\u62D2\u7EDD\u5BA2\u6237\u7AEF\u5220\u9664 Storage", "userId", ctx.userId);
  throw new Error("\u5BA2\u6237\u7AEF\u7981\u6B62\u5220\u9664 Storage");
};

// src/rpc/ping.ts
var rpcPing = function(ctx, logger, nk, params) {
  const rows = nk.sqlQuery("SELECT 1");
  return JSON.stringify({
    ok: true,
    module: "zombieking-p2",
    dbReachable: rows.length === 1,
    userId: ctx.userId || null
  });
};

// src/common/api.ts
var UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
function parseInput(params) {
  if (!params) {
    return {};
  }
  try {
    return JSON.parse(params);
  } catch (e) {
    throw new Error("INVALID_INPUT: params \u5FC5\u987B\u662F JSON");
  }
}
function requireIdem(input) {
  const k = input.idempotency_key;
  if (typeof k !== "string" || !UUID_RE.test(k)) {
    throw new Error("INVALID_INPUT: \u7F3A\u5C11 idempotency_key(UUID)");
  }
  return k;
}
function requireUuid(v, name) {
  if (typeof v !== "string" || !UUID_RE.test(v)) {
    throw new Error("INVALID_INPUT: " + name + " \u5FC5\u987B\u662F UUID");
  }
  return v;
}
function bytesToStr(v) {
  if (typeof v === "string") {
    return v;
  }
  if (v === null || v === void 0) {
    return "";
  }
  let a = v;
  if (typeof a.length !== "number" && typeof a.byteLength === "number") {
    a = new Uint8Array(a);
  }
  let s = "";
  for (let i = 0; i < a.length; i++) {
    s += String.fromCharCode(a[i]);
  }
  return s;
}
function decodeJsonb(v) {
  if (v === null || v === void 0) {
    return null;
  }
  if (typeof v === "string") {
    return JSON.parse(v);
  }
  if (typeof v === "object" && typeof v.length === "number") {
    let s = "";
    for (let i = 0; i < v.length; i++) {
      s += String.fromCharCode(v[i]);
    }
    return JSON.parse(s);
  }
  return v;
}
function callFn(nk, name, args) {
  const holders = [];
  for (let i = 0; i < args.length; i++) {
    holders.push("$" + (i + 1));
  }
  const rows = nk.sqlQuery("SELECT game." + name + "(" + holders.join(",") + ") AS res", args);
  if (!rows || rows.length === 0) {
    throw new Error("FN_NO_RESULT: " + name);
  }
  return decodeJsonb(rows[0].res);
}
function failIfErr(res) {
  if (res && res.err) {
    throw new Error(res.err);
  }
  return res;
}
function rollFloat(nk) {
  const hex = nk.uuidv4().replace(/-/g, "");
  return parseInt(hex.substring(0, 8), 16) / 4294967295;
}
function weightedPick(nk, items) {
  let total = 0;
  for (let i = 0; i < items.length; i++) {
    total += items[i].weight;
  }
  let r = rollFloat(nk) * total;
  for (let i = 0; i < items.length; i++) {
    r -= items[i].weight;
    if (r < 0) {
      return items[i];
    }
  }
  return items[items.length - 1];
}
function parseJson(v, fallback) {
  if (v === null || v === void 0 || v === "") {
    return fallback;
  }
  const decoded = decodeJsonb(v);
  if (decoded !== null && typeof decoded === "object") {
    return decoded;
  }
  try {
    return JSON.parse(String(v));
  } catch (e) {
    return fallback;
  }
}

// src/common/rateLimit.ts
function rateCfg(nk, key, def) {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  const v = rows.length ? parseInt(String(rows[0].value), 10) : def;
  return isNaN(v) ? def : v;
}
function checkRate(nk, rpcId, userId, limitPerMin) {
  if (limitPerMin <= 0) {
    return true;
  }
  const rows = nk.sqlQuery(
    "INSERT INTO game.rate_limit(key, window_start, count) VALUES ($1, date_trunc('minute', now()), 1) ON CONFLICT (key, window_start) DO UPDATE SET count = game.rate_limit.count + 1 RETURNING count",
    [rpcId + ":" + userId]
  );
  if (!rows.length) {
    return true;
  }
  return parseInt(String(rows[0].count), 10) <= limitPerMin;
}
function guardRate(nk, rpcId, userId, cfgKey, def) {
  if (!checkRate(nk, rpcId, userId, rateCfg(nk, cfgKey, def))) {
    throw new Error("RATE_LIMITED: \u64CD\u4F5C\u8FC7\u4E8E\u9891\u7E41\uFF0C\u8BF7\u7A0D\u540E\u518D\u8BD5");
  }
}

// src/rpc/shop.ts
var rpcShopBuy = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_shop_buy", ctx.userId, "rate_asset_per_min", 30);
  }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const itemId = String(input.item_def_id || "");
  const qty = parseInt(String(input.qty), 10);
  const currency = String(input.currency || "gold");
  if (!itemId || !(qty >= 1 && qty <= 99)) {
    throw new Error("INVALID_INPUT: item_def_id/qty");
  }
  const defs = nk.sqlQuery(
    "SELECT price_json->>$1 AS unit FROM game.item_def WHERE id=$2 AND enabled",
    [currency, itemId]
  );
  if (!defs.length || defs[0].unit === null || defs[0].unit === void 0) {
    throw new Error("ITEM_UNAVAILABLE");
  }
  const amount = parseInt(String(defs[0].unit), 10) * qty;
  const res = failIfErr(callFn(nk, "fn_shop_buy", [ctx.userId, itemId, qty, currency, amount, idem]));
  logger.info("shop_buy", "user", ctx.userId, "item", itemId, "qty", qty);
  return JSON.stringify(res);
};

// src/rpc/gacha.ts
var rpcGachaDraw = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_gacha_draw", ctx.userId, "rate_asset_per_min", 30);
  }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const poolId = String(input.pool_id || "");
  const count = parseInt(String(input.count), 10);
  if (!poolId || count !== 1 && count !== 10) {
    throw new Error("INVALID_INPUT: pool_id/count(1|10)");
  }
  const pools = nk.sqlQuery(
    "SELECT currency, cost, items_json, pity FROM game.gacha_pool WHERE id=$1 AND enabled",
    [poolId]
  );
  if (!pools.length) {
    throw new Error("POOL_UNAVAILABLE");
  }
  const pool = pools[0];
  const items = parseJson(pool.items_json, []);
  const pityCfg = parseJson(pool.pity, {});
  const currency = String(pool.currency);
  const unitCost = parseInt(String(pool.cost), 10);
  const pityRows = nk.sqlQuery(
    "SELECT legendary, epic FROM game.gacha_pity WHERE user_id=$1 AND pool_id=$2",
    [ctx.userId, poolId]
  );
  let leg = pityRows.length ? parseInt(String(pityRows[0].legendary), 10) : 0;
  let epic = pityRows.length ? parseInt(String(pityRows[0].epic), 10) : 0;
  const legItems = [];
  const epicItems = [];
  for (let i = 0; i < items.length; i++) {
    if (items[i].rarity >= 3) {
      legItems.push(items[i]);
    }
    if (items[i].rarity >= 2) {
      epicItems.push(items[i]);
    }
  }
  const results = [];
  for (let n = 0; n < count; n++) {
    leg += 1;
    epic += 1;
    let picked;
    if (pityCfg.legendary_at && leg >= pityCfg.legendary_at && legItems.length > 0) {
      picked = weightedPick(nk, legItems);
    } else if (pityCfg.epic_at && epic >= pityCfg.epic_at && epicItems.length > 0) {
      picked = weightedPick(nk, epicItems);
    } else {
      picked = weightedPick(nk, items);
    }
    if (picked.rarity >= 3) {
      leg = 0;
      epic = 0;
    } else if (picked.rarity >= 2) {
      epic = 0;
    }
    results.push({ item_def_id: picked.item_def_id, rarity: picked.rarity });
  }
  const seed = nk.uuidv4();
  const res = failIfErr(callFn(nk, "fn_gacha_draw", [
    ctx.userId,
    poolId,
    currency,
    unitCost * count,
    JSON.stringify(results),
    seed,
    JSON.stringify({ legendary: leg, epic }),
    idem
  ]));
  logger.info("gacha_draw", "user", ctx.userId, "pool", poolId, "count", count);
  return JSON.stringify(res);
};

// src/rpc/enhance.ts
var rpcItemEnhance = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_item_enhance", ctx.userId, "rate_asset_per_min", 30);
  }
  const input = parseInput(params);
  const idem = requireIdem(input);
  const itemId = parseInt(String(input.inventory_item_id), 10);
  if (!(itemId > 0)) {
    throw new Error("INVALID_INPUT: inventory_item_id");
  }
  const items = nk.sqlQuery(
    "SELECT i.level, d.type FROM game.inventory i JOIN game.item_def d ON d.id=i.item_def_id WHERE i.id=$1 AND i.user_id=$2 AND i.status IN ('in_bag','equipped')",
    [itemId, ctx.userId]
  );
  if (!items.length) {
    throw new Error("ITEM_STATE_INVALID");
  }
  const level = parseInt(String(items[0].level), 10);
  const rules = nk.sqlQuery(
    "SELECT to_level, success_rate, cost_json, mats_json, fail_penalty FROM game.enhance_rule WHERE target_type=$1 AND from_level=$2 AND enabled ORDER BY to_level LIMIT 1",
    [String(items[0].type), level]
  );
  if (!rules.length) {
    throw new Error("MAX_LEVEL");
  }
  const rule = rules[0];
  const matReqs = parseJson(rule.mats_json, []);
  const matIds = [];
  for (let m = 0; m < matReqs.length; m++) {
    const req = matReqs[m];
    const found = nk.sqlQuery(
      "SELECT id FROM game.inventory WHERE user_id=$1 AND item_def_id=$2 AND status='in_bag' ORDER BY id LIMIT $3",
      [ctx.userId, req.item_def_id, req.count]
    );
    if (found.length < req.count) {
      throw new Error("MATERIALS_SHORT: " + req.item_def_id);
    }
    for (let f = 0; f < found.length; f++) {
      matIds.push(parseInt(String(found[f].id), 10));
    }
  }
  const cost = parseJson(rule.cost_json, { currency: "gold", amount: 0 });
  const success = rollFloat(nk) < parseFloat(String(rule.success_rate));
  const penalty = String(rule.fail_penalty);
  const newLevel = success ? parseInt(String(rule.to_level), 10) : penalty === "downgrade" ? Math.max(0, level - 1) : level;
  const res = failIfErr(callFn(nk, "fn_item_enhance", [
    ctx.userId,
    itemId,
    level,
    newLevel,
    JSON.stringify(matIds),
    cost.currency,
    cost.amount || 0,
    idem
  ]));
  res.success = success;
  logger.info("item_enhance", "user", ctx.userId, "item", itemId, "success", success);
  return JSON.stringify(res);
};

// src/rpc/loadout.ts
var VALID_MODES = { tdm_5v5: true, zombie_infection: true };
var rpcLoadoutSet = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_loadout_set", ctx.userId, "rate_asset_per_min", 30);
  }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES[mode]) {
    throw new Error("INVALID_INPUT: mode");
  }
  const slots = input.slots || {};
  const keys = Object.keys(slots);
  if (keys.length === 0 || keys.length > 8) {
    throw new Error("INVALID_INPUT: slots");
  }
  for (let i = 0; i < keys.length; i++) {
    const slotName = keys[i];
    const invId = parseInt(String(slots[slotName]), 10);
    if (!(invId > 0)) {
      throw new Error("INVALID_INPUT: slots." + slotName);
    }
    const ok = nk.sqlQuery(
      "SELECT 1 AS x FROM game.inventory i JOIN game.item_def d ON d.id=i.item_def_id WHERE i.id=$1 AND i.user_id=$2 AND i.status IN ('in_bag','equipped') AND d.slot=$3 AND $4 = ANY(d.mode_tags) AND d.enabled",
      [invId, ctx.userId, slotName, mode]
    );
    if (!ok.length) {
      throw new Error("SLOT_INCOMPATIBLE: " + slotName);
    }
  }
  const res = failIfErr(callFn(nk, "fn_loadout_set", [ctx.userId, mode, JSON.stringify(slots)]));
  logger.info("loadout_set", "user", ctx.userId, "mode", mode);
  return JSON.stringify(res);
};

// src/rpc/bootstrap.ts
var rpcBootstrapGet = function(ctx, logger, nk, params) {
  const wallet = nk.sqlQuery(
    "SELECT currency, balance FROM game.wallet WHERE user_id=$1 ORDER BY currency",
    [ctx.userId]
  );
  const inventory = nk.sqlQuery(
    "SELECT id, item_def_id, quality, level, status FROM game.inventory WHERE user_id=$1 AND status <> 'consumed' ORDER BY id",
    [ctx.userId]
  );
  const loadouts = nk.sqlQuery(
    "SELECT mode, slots::text AS slots, version FROM game.loadout WHERE user_id=$1 ORDER BY mode",
    [ctx.userId]
  );
  for (let i = 0; i < loadouts.length; i++) {
    loadouts[i].slots = decodeJsonb(loadouts[i].slots);
  }
  return JSON.stringify({ wallet, inventory, loadouts });
};

// src/rpc/catalog.ts
function decodeFields(rows, fields) {
  for (let i = 0; i < rows.length; i++) {
    for (let j = 0; j < fields.length; j++) {
      rows[i][fields[j]] = decodeJsonb(rows[i][fields[j]]);
    }
  }
}
var rpcCatalogGet = function(ctx, logger, nk, params) {
  const items = nk.sqlQuery(
    "SELECT id, name, type, rarity, price_json::text AS price_json, mode_tags::text AS mode_tags, slot FROM game.item_def WHERE enabled ORDER BY sort"
  );
  decodeFields(items, ["price_json"]);
  for (let i = 0; i < items.length; i++) {
    const mt = String(items[i].mode_tags || "");
    items[i].mode_tags = mt === "{}" ? [] : mt.replace(/[{}]/g, "").split(",");
  }
  const pools = nk.sqlQuery(
    "SELECT id, name, currency, cost, items_json::text AS items_json, pity::text AS pity FROM game.gacha_pool WHERE enabled ORDER BY id"
  );
  decodeFields(pools, ["items_json", "pity"]);
  const rules = nk.sqlQuery(
    "SELECT id, target_type, from_level, to_level, success_rate, cost_json::text AS cost_json, mats_json::text AS mats_json, fail_penalty FROM game.enhance_rule WHERE enabled ORDER BY target_type, from_level"
  );
  decodeFields(rules, ["cost_json", "mats_json"]);
  return JSON.stringify({ items, pools, rules });
};

// src/rpc/gm.ts
var rpcGmGrantCurrency = function(ctx, logger, nk, params) {
  let devMode = "false";
  const env = ctx.environment;
  if (env && String(env.DEV_MODE) === "true") {
    devMode = "true";
  } else {
    const cfg = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key='dev_mode'");
    if (cfg.length) {
      devMode = String(cfg[0].value);
    }
  }
  if (devMode !== "true") {
    throw new Error("DEV_MODE_REQUIRED");
  }
  let roles = [];
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
  if (!currency || !(amount >= 1 && amount <= 1e6)) {
    throw new Error("INVALID_INPUT: currency/amount(1..1000000)");
  }
  const res = failIfErr(callFn(nk, "fn_gm_grant", [target, currency, amount, ctx.username || ctx.userId, idem]));
  logger.warn("gm_grant", "operator", ctx.username, "target", target, "currency", currency, "amount", amount);
  return JSON.stringify(res);
};

// src/common/hmac.ts
function bytesToHex(buf) {
  let view = buf;
  if (typeof ArrayBuffer !== "undefined" && buf instanceof ArrayBuffer) {
    view = new Uint8Array(buf);
  }
  let s = "";
  const len = view.length !== void 0 ? view.length : view.byteLength;
  for (let i = 0; i < len; i++) {
    const b = view[i];
    s += (b < 16 ? "0" : "") + b.toString(16);
  }
  return s;
}
function hmacHex(nk, input, key) {
  return bytesToHex(nk.hmacSha256Hash(input, key));
}
function settleCanon(matchId, dsId, settledAt, payloadHash) {
  return matchId + "|" + dsId + "|" + settledAt + "|" + payloadHash;
}
function verifySettleSig(nk, p, dsSecret) {
  const canon = settleCanon(String(p.match_id), String(p.ds_id), Number(p.settled_at), String(p.payload_hash));
  const expect = hmacHex(nk, canon, dsSecret);
  const got = String(p.sig || "");
  if (expect.length !== got.length) {
    return false;
  }
  let diff = 0;
  for (let i = 0; i < expect.length; i++) {
    diff |= expect.charCodeAt(i) ^ got.charCodeAt(i);
  }
  return diff === 0;
}

// src/rpc/match.ts
var MODES = { tdm_5v5: true, zombie_infection: true };
var GRACE_SEC = 300;
function isDevMode(nk) {
  const cfg = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key='dev_mode'");
  return cfg.length > 0 && String(cfg[0].value) === "true";
}
function audit(nk, actor, action, target, detailJson) {
  nk.sqlQuery(
    "INSERT INTO game.audit_log(actor,action,target,detail) VALUES ($1,$2,$3,$4::jsonb)",
    [actor, action, target, detailJson]
  );
}
var rpcMatchCreate = function(ctx, logger, nk, params) {
  if (!isDevMode(nk)) {
    throw new Error("DEV_ONLY_ENTRY");
  }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!MODES[mode]) {
    throw new Error("INVALID_INPUT: mode");
  }
  const players = input.players || [];
  if (players.length < 2 || players.length > 10) {
    throw new Error("INVALID_INPUT: players");
  }
  const duration = Math.min(3600, Math.max(120, parseInt(String(input.duration_sec || 900), 10)));
  const matchId = nk.uuidv4();
  const dsId = nk.uuidv4();
  const dsSecret = nk.uuidv4().replace(/-/g, "") + nk.uuidv4().replace(/-/g, "");
  const nowSec = Math.floor(Date.now() / 1e3);
  const expireAt = nowSec + duration + GRACE_SEC;
  const set = [];
  for (let i = 0; i < players.length; i++) {
    set.push({ user_id: requireUuid(players[i].user_id, "players[" + i + "].user_id"), team: parseInt(String(players[i].team), 10) });
  }
  nk.sqlQuery(
    "INSERT INTO game.match_record(match_id,mode,map,ds_id,ds_host_id,player_set,started_at,expire_at,status,ds_secret) VALUES ($1,$2,$3,$4,'dev-host',$5::jsonb,to_timestamp($6),to_timestamp($7),'pending',$8)",
    [matchId, mode, String(input.map || "dm_simple"), dsId, JSON.stringify(set), nowSec, expireAt, dsSecret]
  );
  const tokens = [];
  for (let i = 0; i < set.length; i++) {
    const lo = nk.sqlQuery("SELECT slots::text AS slots FROM game.loadout WHERE user_id=$1 AND mode=$2", [set[i].user_id, mode]);
    const snapshot = lo.length ? decodeJsonb(lo[0].slots) : {};
    const payload = JSON.stringify({
      match_id: matchId,
      user_id: set[i].user_id,
      team: set[i].team,
      loadout: snapshot,
      issued_at: nowSec,
      expire_at: expireAt
    });
    tokens.push({ user_id: set[i].user_id, join_token: payload + "." + hmacHex(nk, payload, dsSecret) });
  }
  logger.info("match_created", "match", matchId, "mode", mode, "players", set.length);
  return JSON.stringify({
    match_id: matchId,
    ds_id: dsId,
    ds_ip: "127.0.0.1",
    ds_port: 7777,
    mode,
    expire_at: expireAt,
    tokens
  });
};
var rpcMatchSettle = function(ctx, logger, nk, params) {
  const p = parseInput(params);
  const matchId = requireUuid(p.match_id, "match_id");
  const recs = nk.sqlQuery(
    "SELECT status, ds_id::text AS ds_id, ds_secret, mode, player_set::text AS player_set, extract(epoch from started_at)::bigint AS started_epoch, extract(epoch from expire_at)::bigint AS expire_epoch FROM game.match_record WHERE match_id=$1",
    [matchId]
  );
  if (!recs.length) {
    throw new Error("MATCH_NOT_FOUND");
  }
  const rec = recs[0];
  if (String(rec.status) !== "pending") {
    return JSON.stringify({ ok: true, replayed: true });
  }
  const dsSecret = rec.ds_secret ? String(rec.ds_secret) : "";
  if (!dsSecret) {
    throw new Error("SECRET_CLEARED");
  }
  const fail = function(code) {
    audit(nk, "ds", "settle_rejected", matchId, JSON.stringify({ code, ds_id: String(p.ds_id || "") }));
    nk.sqlQuery("UPDATE game.ds_host hs SET suspicious_count=suspicious_count+1 FROM game.match_record mr WHERE mr.match_id=$1 AND hs.host_id=mr.ds_host_id", [matchId]);
    throw new Error(code);
  };
  if (!verifySettleSig(nk, p, dsSecret)) {
    fail("SETTLE_SIG_INVALID");
  }
  if (String(p.ds_id) !== String(rec.ds_id)) {
    fail("SETTLE_DS_MISMATCH");
  }
  const settledAt = Number(p.settled_at);
  if (!(settledAt >= Number(rec.started_epoch) - 60 && settledAt <= Number(rec.expire_epoch))) {
    fail("SETTLE_TIME_WINDOW");
  }
  const expected = decodeJsonb(rec.player_set);
  const got = p.players || [];
  if (got.length !== expected.length) {
    fail("SETTLE_PLAYER_SET");
  }
  const expMap = {};
  for (let i = 0; i < expected.length; i++) {
    expMap[String(expected[i].user_id)] = parseInt(String(expected[i].team), 10);
  }
  const gotSet = {};
  for (let i = 0; i < got.length; i++) {
    const uid = String(got[i].user_id);
    if (expMap[uid] === void 0 || expMap[uid] !== parseInt(String(got[i].team), 10) || gotSet[uid]) {
      fail("SETTLE_PLAYER_SET");
    }
    gotSet[uid] = true;
  }
  const mode = String(rec.mode);
  const results = [];
  const rewards = [];
  const rating = [];
  const tdmKills = {};
  const tdmDeaths = {};
  const winnerTeam = p.outcome ? parseInt(String(p.outcome.winner_team), 10) : -1;
  const winnerFaction = p.outcome ? String(p.outcome.winner_faction || "") : "";
  for (let i = 0; i < got.length; i++) {
    const pl = got[i];
    const st = pl.stats || {};
    const kills = Math.max(0, parseInt(String(st.kills || 0), 10));
    const deaths = Math.max(0, parseInt(String(st.deaths || 0), 10));
    if (kills > 200 || deaths > 200) {
      fail("SETTLE_STATS_RANGE");
    }
    let won = false;
    if (mode === "tdm_5v5") {
      const team = parseInt(String(pl.team), 10);
      tdmKills[team] = (tdmKills[team] || 0) + kills;
      tdmDeaths[team] = (tdmDeaths[team] || 0) + deaths;
      won = team === winnerTeam;
    } else {
      const role = String(st.role || "");
      if (role !== "human" && role !== "zombie") {
        fail("SETTLE_ROLE_INVALID");
      }
      won = role === winnerFaction;
    }
    results.push({ user_id: String(pl.user_id), team: parseInt(String(pl.team), 10), stats: st });
    const gold = 200 + kills * 20 + (won ? 500 : 0);
    rewards.push({ user_id: String(pl.user_id), currency: "gold", amount: gold });
    rating.push({ user_id: String(pl.user_id), mode, delta: won ? 25 : -25, won });
  }
  if (mode === "tdm_5v5") {
    const teams = Object.keys(tdmKills);
    if (teams.length !== 2) {
      fail("SETTLE_TEAMS");
    }
    const t1 = teams[0];
    const t2 = teams[1];
    if (tdmDeaths[t1] !== tdmKills[t2] || tdmDeaths[t2] !== tdmKills[t1]) {
      fail("SETTLE_CROSSCHECK");
    }
  } else if (mode === "zombie_infection") {
    if (winnerFaction !== "human" && winnerFaction !== "zombie") {
      fail("SETTLE_OUTCOME_INVALID");
    }
    const events = p.outcome && p.outcome.infection_events || [];
    for (let i = 1; i < events.length; i++) {
      if (Number(events[i].t_sec) < Number(events[i - 1].t_sec)) {
        fail("SETTLE_EVENTS_ORDER");
      }
    }
    const zombieSet = {};
    for (let i = 0; i < got.length; i++) {
      if (String((got[i].stats || {}).role) === "zombie") {
        zombieSet[String(got[i].user_id)] = true;
      }
    }
    const eventSet = {};
    for (let i = 0; i < events.length; i++) {
      const uid = String(events[i].user_id);
      if (!zombieSet[uid] || Number(events[i].t_sec) < 0 || eventSet[uid]) {
        fail("SETTLE_EVENTS_MISMATCH");
      }
      eventSet[uid] = true;
    }
    if (Object.keys(eventSet).length !== Object.keys(zombieSet).length) {
      fail("SETTLE_EVENTS_MISMATCH");
    }
  }
  const res = failIfErr(callFn(nk, "fn_match_settle", [
    matchId,
    JSON.stringify(results),
    JSON.stringify(rewards),
    JSON.stringify(rating),
    String(p.payload_hash)
  ]));
  logger.info("match_settled", "match", matchId, "players", got.length);
  return JSON.stringify(res);
};

// src/common/alloc.ts
var VALID_MODES2 = { tdm_5v5: true, zombie_infection: true };
var GRACE_SEC2 = 300;
function sysConfig(nk, key) {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : "";
}
function createMatchForGroup(nk, logger, mode, userIds) {
  if (!VALID_MODES2[mode] || userIds.length < 2) {
    return "";
  }
  const managerUrl = sysConfig(nk, "ds_manager_url");
  const serviceToken = sysConfig(nk, "ds_service_token");
  if (!managerUrl || !serviceToken) {
    logger.warn("alloc: ds_manager not configured");
    return "";
  }
  const half = Math.ceil(userIds.length / 2);
  const flip = rollFloat(nk) < 0.5 ? 1 : 0;
  const players = [];
  for (let i = 0; i < userIds.length; i++) {
    players.push({ user_id: userIds[i], team: i < half ? 1 + flip : 2 - flip });
  }
  const matchId = nk.uuidv4();
  const duration = 900;
  const allocBody = JSON.stringify({
    match_id: matchId,
    mode,
    map: mode === "tdm_5v5" ? "dm_simple" : "zc_infection",
    duration_sec: duration,
    players
  });
  let resp;
  try {
    resp = nk.httpRequest(
      managerUrl + "/v1/allocations",
      "post",
      { "Content-Type": "application/json", "X-Service-Token": serviceToken },
      allocBody,
      4e3
    );
  } catch (e) {
    logger.warn("alloc: ds manager unreachable", String(e));
    return "";
  }
  if (resp.code !== 200) {
    logger.warn("alloc: failed code=" + resp.code + " body=" + resp.body);
    return "";
  }
  const alloc = JSON.parse(resp.body);
  const nowSec = Math.floor(Date.now() / 1e3);
  const expireAt = nowSec + duration + GRACE_SEC2;
  nk.sqlQuery(
    "INSERT INTO game.match_record(match_id,mode,map,ds_id,ds_host_id,player_set,started_at,expire_at,status,ds_secret) VALUES ($1,$2,$3,$4,$5,$6::jsonb,to_timestamp($7),to_timestamp($8),'pending',$9)",
    [matchId, mode, alloc.map || "", alloc.ds_id, alloc.ds_host_id, JSON.stringify(players), nowSec, expireAt, alloc.ds_secret]
  );
  const tokens = [];
  for (let i = 0; i < players.length; i++) {
    const lo = nk.sqlQuery("SELECT slots::text AS slots FROM game.loadout WHERE user_id=$1 AND mode=$2", [players[i].user_id, mode]);
    let snapshot = {};
    try {
      snapshot = lo.length ? JSON.parse(String(lo[0].slots)) : {};
    } catch (e) {
      snapshot = {};
    }
    const payload = JSON.stringify({
      match_id: matchId,
      user_id: players[i].user_id,
      team: players[i].team,
      loadout: snapshot,
      issued_at: nowSec,
      expire_at: expireAt
    });
    const token = payload + "." + hmacHex(nk, payload, alloc.ds_secret);
    tokens.push({ user_id: players[i].user_id, join_token: token });
    nk.sqlQuery("UPDATE game.match_queue SET match_id=$1, join_token=$2 WHERE user_id=$3", [matchId, token, players[i].user_id]);
  }
  logger.info("alloc ok", "match", matchId, "mode", mode, "players", players.length, "ds", alloc.ds_id);
  nk.matchCreate("zk_match", {
    match_id: matchId,
    ds_ip: alloc.ds_ip,
    ds_port: alloc.ds_port,
    mode,
    expire_at: expireAt,
    tokens
  });
  return matchId;
}
var zkMatchInit = function(ctx, logger, nk, params) {
  return { state: { meta: params, broadcasted: false }, tickRate: 1, label: "zk_match" };
};
var zkMatchJoinAttempt = function(ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  return { state, accept: true };
};
var zkMatchJoin = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state };
};
var zkMatchLeave = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state };
};
var zkMatchLoop = function(ctx, logger, nk, dispatcher, tick, state, messages) {
  if (!state.broadcasted) {
    dispatcher.broadcastMessage(100, JSON.stringify(state.meta), null, null, true);
    state.broadcasted = true;
  }
  return { state };
};
var zkMatchTerminate = function(ctx, logger, nk, dispatcher, tick, graceSeconds) {
  return {};
};
var zkMatchSignal = function(ctx, logger, nk, dispatcher, tick, state, data) {
  return { state, data };
};
var zkMatchHandlers = {
  matchInit: zkMatchInit,
  matchJoinAttempt: zkMatchJoinAttempt,
  matchJoin: zkMatchJoin,
  matchLeave: zkMatchLeave,
  matchLoop: zkMatchLoop,
  matchTerminate: zkMatchTerminate,
  matchSignal: zkMatchSignal
};

// src/rpc/queue.ts
var VALID_MODES3 = { tdm_5v5: true, zombie_infection: true };
function sysCfg(nk, key, def) {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : def;
}
var rpcQueueEnter = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_queue_enter", ctx.userId, "rate_asset_per_min", 30);
  }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES3[mode]) {
    throw new Error("INVALID_INPUT: mode");
  }
  const mmrRows = nk.sqlQuery("SELECT mmr FROM game.player_rating WHERE user_id=$1 AND mode=$2", [ctx.userId, mode]);
  const mmr = mmrRows.length ? parseInt(String(mmrRows[0].mmr), 10) : 1e3;
  nk.sqlQuery(
    "INSERT INTO game.match_queue(user_id,mode,mmr,queued_at,match_id,join_token) VALUES ($1,$2,$3,now(),NULL,NULL) ON CONFLICT (user_id) DO UPDATE SET mode=EXCLUDED.mode, mmr=EXCLUDED.mmr, queued_at=now(), match_id=NULL, join_token=NULL, party_id=NULL",
    [ctx.userId, mode, mmr]
  );
  ensureQueueProcessor(nk, logger);
  return JSON.stringify({ ok: true, mode, mmr });
};
function ensureQueueProcessor(nk, logger) {
  let procId = sysCfg(nk, "queue_proc_match_id", "");
  let alive = false;
  if (procId) {
    try {
      const m = nk.matchGet(procId);
      alive = !!m;
    } catch (e) {
      alive = false;
    }
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
var rpcQueueCancel = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_queue_cancel", ctx.userId, "rate_asset_per_min", 30);
  }
  const del = nk.sqlQuery("DELETE FROM game.match_queue WHERE user_id=$1 AND match_id IS NULL RETURNING user_id", [ctx.userId]);
  return JSON.stringify({ ok: true, cancelled: del.length > 0 });
};
var rpcQueueStatus = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_queue_status", ctx.userId, "rate_lobby_per_min", 120);
  }
  const rows = nk.sqlQuery(
    "SELECT match_id::text AS match_id, join_token FROM game.match_queue WHERE user_id=$1",
    [ctx.userId]
  );
  if (!rows.length) {
    return JSON.stringify({ queued: false });
  }
  if (!rows[0].match_id) {
    return JSON.stringify({ queued: true, matched: false });
  }
  return JSON.stringify({
    queued: true,
    matched: true,
    match_id: String(rows[0].match_id),
    join_token: String(rows[0].join_token)
  });
};
var zkQueueProcInit = function(ctx, logger, nk, params) {
  return { state: { tick: 0 }, tickRate: 1, label: "zk_queueproc" };
};
var zkQueueProcJoinAttempt = function(ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  return { state, accept: false };
};
var zkQueueProcJoin = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state };
};
var zkQueueProcLeave = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  return { state };
};
var zkQueueProcSignal = function(ctx, logger, nk, dispatcher, tick, state, data) {
  return { state, data };
};
var zkQueueProcTerminate = function(ctx, logger, nk, dispatcher, tick, graceSeconds) {
  return {};
};
var zkQueueProcLoop = function(ctx, logger, nk, dispatcher, tick, state, messages) {
  state.tick = (state.tick || 0) + 1;
  if (state.tick % 10 === 0) {
    const voided = nk.sqlQuery(
      "UPDATE game.match_record SET status='void' WHERE status='pending' AND expire_at < now() RETURNING match_id::text AS mid"
    );
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
    "SELECT user_id::text AS uid, mode, mmr, party_id::text AS party, extract(epoch from (now()-queued_at))::int AS waited FROM game.match_queue WHERE match_id IS NULL ORDER BY mode, mmr"
  );
  if (!rows.length) {
    return { state };
  }
  const byMode = {};
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    if (!byMode[r.mode]) {
      byMode[r.mode] = [];
    }
    byMode[r.mode].push(r);
  }
  const modes = Object.keys(byMode);
  for (let mi = 0; mi < modes.length; mi++) {
    const mode = modes[mi];
    const need = mode === "tdm_5v5" ? minTdm : minZom;
    const parties = {};
    const solos = [];
    const arr = byMode[mode];
    for (let i2 = 0; i2 < arr.length; i2++) {
      const r = arr[i2];
      if (r.party) {
        if (!parties[r.party]) {
          parties[r.party] = [];
        }
        parties[r.party].push(r);
      } else {
        solos.push(r);
      }
    }
    const pids = Object.keys(parties);
    for (let pi = 0; pi < pids.length; pi++) {
      const grp = parties[pids[pi]];
      const ids = [];
      for (let k = 0; k < grp.length; k++) {
        ids.push(String(grp[k].uid));
      }
      if (grp.length < need) {
        const lack = need - grp.length;
        if (solos.length < lack) {
          continue;
        }
        for (let k = 0; k < lack; k++) {
          ids.push(String(solos.shift().uid));
        }
      }
      const mid = createMatchForGroup(nk, logger, mode, ids);
      if (!mid) {
        break;
      }
    }
    let i = 0;
    while (i + need <= solos.length) {
      const j = i + need - 1;
      let maxWait = 0;
      for (let k = i; k <= j; k++) {
        if (solos[k].waited > maxWait) {
          maxWait = solos[k].waited;
        }
      }
      const win = winBase + 50 * Math.floor(maxWait / 10);
      if (Number(solos[j].mmr) - Number(solos[i].mmr) <= win) {
        const ids = [];
        for (let k = i; k < i + need; k++) {
          ids.push(String(solos[k].uid));
        }
        const mid = createMatchForGroup(nk, logger, mode, ids);
        if (!mid) {
          break;
        }
        i += need;
      } else {
        i += 1;
      }
    }
  }
  return { state };
};
var zkQueueProcHandlers = {
  matchInit: zkQueueProcInit,
  matchJoinAttempt: zkQueueProcJoinAttempt,
  matchJoin: zkQueueProcJoin,
  matchLeave: zkQueueProcLeave,
  matchLoop: zkQueueProcLoop,
  matchTerminate: zkQueueProcTerminate,
  matchSignal: zkQueueProcSignal
};
var matchmakerMatched = function(ctx, logger, nk, entries) {
  if (!entries || entries.length < 2) {
    return null;
  }
  const mode = String(entries[0].properties.mode || "");
  if (!VALID_MODES3[mode]) {
    return null;
  }
  const ids = [];
  for (let i = 0; i < entries.length; i++) {
    ids.push(String(entries[i].presence.userId));
  }
  const mid = createMatchForGroup(nk, logger, mode, ids);
  return mid ? mid : null;
};

// src/rpc/account.ts
var EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;
var MIN_PWD = 8;
var MAX_PWD = 100;
var rpcAccountBindEmail = function(ctx, logger, nk, params) {
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  guardRate(nk, "rpc_account_bind_email", ctx.userId, "rate_account_per_min", 5);
  const input = parseInput(params);
  const email = String(input.email || "").trim().toLowerCase();
  const password = String(input.password || "");
  if (!EMAIL_RE.test(email)) {
    throw new Error("INVALID_INPUT: email \u683C\u5F0F\u4E0D\u6B63\u786E");
  }
  if (password.length < MIN_PWD || password.length > MAX_PWD) {
    throw new Error("INVALID_INPUT: \u5BC6\u7801\u957F\u5EA6\u9700\u4E3A " + MIN_PWD + "-" + MAX_PWD + " \u4F4D");
  }
  const rows = nk.sqlQuery("SELECT email FROM users WHERE id=$1", [ctx.userId]);
  if (rows.length && rows[0].email) {
    throw new Error("ALREADY_BOUND: \u8BE5\u8D26\u53F7\u5DF2\u7ED1\u5B9A\u90AE\u7BB1");
  }
  try {
    nk.linkEmail(ctx.userId, email, password);
  } catch (e) {
    const msg = String(e && e.message ? e.message : e);
    logger.warn("linkEmail failed", "user", ctx.userId, "err", msg);
    if (/exist|use|conflict/i.test(msg)) {
      throw new Error("EMAIL_IN_USE: \u8BE5\u90AE\u7BB1\u5DF2\u88AB\u4F7F\u7528");
    }
    throw new Error("BIND_FAILED: " + msg);
  }
  logger.info("email bound", "user", ctx.userId);
  return JSON.stringify({ ok: true, email });
};

// src/rpc/lobby.ts
var SHARD_PREFIX = "zk-lobby-";
var SHARD_MAX = 100;
var LOBBY_BOT_DEVICE_ID = "zk-lobby-bot";
var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
function b64ascii(s) {
  let out = "";
  let i = 0;
  while (i < s.length) {
    const c0 = s.charCodeAt(i++);
    const c1 = i < s.length ? s.charCodeAt(i++) : -1;
    const c2 = i < s.length ? s.charCodeAt(i++) : -1;
    out += B64.charAt(c0 >> 2);
    if (c1 === -1) {
      out += B64.charAt((c0 & 3) << 4) + "==";
      break;
    }
    out += B64.charAt((c0 & 3) << 4 | c1 >> 4);
    if (c2 === -1) {
      out += B64.charAt((c1 & 15) << 2) + "=";
      break;
    }
    out += B64.charAt((c1 & 15) << 2 | c2 >> 6);
    out += B64.charAt(c2 & 63);
  }
  return out;
}
function lobbyBotToken(nk) {
  const selfUrl = sysCfg2(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const serverKey = sysCfg2(nk, "socket_server_key", "");
  if (!serverKey) {
    throw new Error("NOT_CONFIGURED: sys_config \u7F3A\u5C11 socket_server_key");
  }
  const login = nk.httpRequest(
    selfUrl + "/v2/account/authenticate/device?create=true",
    "post",
    { "Authorization": "Basic " + b64ascii(serverKey + ":"), "Content-Type": "application/json" },
    JSON.stringify({ id: LOBBY_BOT_DEVICE_ID }),
    3e3
  );
  if (login.code !== 200) {
    throw new Error("LOBBY_BOT_LOGIN_FAILED: code=" + login.code);
  }
  return String(JSON.parse(login.body).token);
}
function createLobbyGroup(nk, logger, name) {
  const selfUrl = sysCfg2(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const create = nk.httpRequest(
    selfUrl + "/v2/group",
    "post",
    { "Authorization": "Bearer " + lobbyBotToken(nk), "Content-Type": "application/json" },
    JSON.stringify({ name, open: true, lang_tag: "en", editable: false, max_count: SHARD_MAX, description: "lobby world channel" }),
    3e3
  );
  if (create.code !== 200) {
    throw new Error("LOBBY_GROUP_CREATE_FAILED: code=" + create.code + " body=" + create.body);
  }
  return String(JSON.parse(create.body).id);
}
function addLobbyGroupUser(nk, groupId, userId) {
  const selfUrl = sysCfg2(nk, "lobby_self_url", "http://127.0.0.1:7350");
  const resp = nk.httpRequest(
    selfUrl + "/v2/group/" + groupId + "/add?user_ids=" + userId,
    "post",
    { "Authorization": "Bearer " + lobbyBotToken(nk) },
    "",
    3e3
  );
  if (resp.code !== 200 && resp.code !== 204) {
    throw new Error("LOBBY_GROUP_ADD_FAILED: code=" + resp.code + " body=" + resp.body);
  }
}
function sysCfg2(nk, key, def) {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  return rows.length ? String(rows[0].value) : def;
}
function sysCfgSet(nk, key, value) {
  nk.sqlQuery(
    "INSERT INTO game.sys_config(key,value,note) VALUES ($1,$2,'lobby channel') ON CONFLICT (key) DO UPDATE SET value=EXCLUDED.value",
    [key, value]
  );
}
var rpcLobbyChannelGet = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_lobby_channel_get", ctx.userId, "rate_lobby_per_min", 120);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  const count = parseInt(sysCfg2(nk, "lobby_channel_count", "0"), 10);
  let gid = "";
  let shard = 0;
  for (let i = 1; i <= count; i++) {
    const cand = sysCfg2(nk, "lobby_group_" + i, "");
    if (!cand) {
      continue;
    }
    const rows = nk.sqlQuery(
      "SELECT count(*)::int AS cnt FROM group_edge WHERE source_id=$1::uuid AND state<=2",
      [cand]
    );
    const cnt = rows.length ? parseInt(String(rows[0].cnt), 10) : 0;
    if (cnt < SHARD_MAX) {
      gid = cand;
      shard = i;
      break;
    }
  }
  if (!gid) {
    shard = count + 1;
    gid = createLobbyGroup(nk, logger, SHARD_PREFIX + shard);
    sysCfgSet(nk, "lobby_group_" + shard, gid);
    sysCfgSet(nk, "lobby_channel_count", String(shard));
    logger.info("lobby shard created", "shard", shard, "group", gid);
  }
  const member = nk.sqlQuery(
    "SELECT 1 AS ok FROM group_edge WHERE source_id=$1::uuid AND destination_id=$2::uuid AND state<=2",
    [gid, ctx.userId]
  );
  if (!member.length) {
    addLobbyGroupUser(nk, gid, ctx.userId);
  }
  return JSON.stringify({ ok: true, group_id: gid, shard });
};
var ONLINE_TTL_SEC = 60;
var rpcLobbyHeartbeat = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_lobby_heartbeat", ctx.userId, "rate_lobby_per_min", 120);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  let uname = ctx.username ? String(ctx.username) : "";
  if (!uname) {
    const rows = nk.sqlQuery("SELECT username FROM users WHERE id=$1", [ctx.userId]);
    uname = rows.length ? String(rows[0].username) : "";
  }
  nk.sqlQuery(
    "INSERT INTO game.online_users(user_id,username,last_seen) VALUES ($1::uuid,$2,now()) ON CONFLICT (user_id) DO UPDATE SET last_seen=now(), username=EXCLUDED.username",
    [ctx.userId, uname]
  );
  return JSON.stringify({ ok: true });
};
var rpcOnlineList = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_online_list", ctx.userId, "rate_lobby_per_min", 120);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  nk.sqlQuery("DELETE FROM game.online_users WHERE last_seen < now() - interval '" + ONLINE_TTL_SEC + " seconds'");
  const rows = nk.sqlQuery(
    "SELECT user_id::text AS uid, username FROM game.online_users WHERE user_id <> $1::uuid ORDER BY username LIMIT 200",
    [ctx.userId]
  );
  const online = [];
  for (let i = 0; i < rows.length; i++) {
    online.push({ user_id: String(rows[i].uid), username: String(rows[i].username) });
  }
  return JSON.stringify({ ok: true, online, ttl_sec: ONLINE_TTL_SEC });
};

// src/rpc/room.ts
var VALID_MODES4 = { tdm_5v5: true, zombie_infection: true };
var MAX_MEMBERS_CAP = 10;
function currentRoomId(nk, userId) {
  const rows = nk.sqlQuery("SELECT room_id::text AS rid FROM game.room_member WHERE user_id=$1::uuid", [userId]);
  return rows.length ? String(rows[0].rid) : "";
}
var rpcRoomCreate = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_room_create", ctx.userId, "rate_asset_per_min", 30);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  const input = parseInput(params);
  const mode = String(input.mode || "");
  if (!VALID_MODES4[mode]) {
    throw new Error("INVALID_INPUT: mode");
  }
  if (currentRoomId(nk, ctx.userId)) {
    throw new Error("IN_ROOM: \u5DF2\u5728\u623F\u95F4\u4E2D\uFF0C\u8BF7\u5148\u79BB\u5F00");
  }
  let max = parseInt(String(input.max_members || MAX_MEMBERS_CAP), 10);
  if (isNaN(max) || max < 2) {
    max = 2;
  }
  if (max > MAX_MEMBERS_CAP) {
    max = MAX_MEMBERS_CAP;
  }
  const map = String(input.map || "").substring(0, 64);
  const roomId = nk.uuidv4();
  nk.sqlQuery(
    "INSERT INTO game.room(room_id,owner_id,mode,map,max_members) VALUES ($1::uuid,$2::uuid,$3,$4,$5)",
    [roomId, ctx.userId, mode, map, max]
  );
  nk.sqlQuery("INSERT INTO game.room_member(room_id,user_id) VALUES ($1::uuid,$2::uuid)", [roomId, ctx.userId]);
  const matchId = nk.matchCreate("zk_lobby", { room_id: roomId });
  nk.sqlQuery("UPDATE game.room SET match_id=$1 WHERE room_id=$2::uuid", [matchId, roomId]);
  logger.info("room created", "room", roomId, "owner", ctx.userId, "match", matchId);
  return JSON.stringify({ ok: true, room_id: roomId, match_id: matchId, mode, map, max_members: max });
};
var rpcRoomJoin = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_room_join", ctx.userId, "rate_asset_per_min", 30);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  const input = parseInput(params);
  const roomId = requireUuid(input.room_id, "room_id");
  const mine = currentRoomId(nk, ctx.userId);
  if (mine) {
    if (mine === roomId) {
      const r = nk.sqlQuery("SELECT match_id AS mid FROM game.room WHERE room_id=$1::uuid", [roomId]);
      return JSON.stringify({ ok: true, room_id: roomId, match_id: r.length ? String(r[0].mid) : "" });
    }
    throw new Error("IN_OTHER_ROOM: \u5DF2\u5728\u5176\u4ED6\u623F\u95F4");
  }
  const rows = nk.sqlQuery(
    "SELECT mode, map, max_members, match_id AS mid, (SELECT count(*) FROM game.room_member m WHERE m.room_id=r.room_id)::int AS members FROM game.room r WHERE room_id=$1::uuid",
    [roomId]
  );
  if (!rows.length) {
    throw new Error("ROOM_NOT_FOUND");
  }
  const room = rows[0];
  if (parseInt(String(room.members), 10) >= parseInt(String(room.max_members), 10)) {
    throw new Error("ROOM_FULL");
  }
  nk.sqlQuery("INSERT INTO game.room_member(room_id,user_id) VALUES ($1::uuid,$2::uuid) ON CONFLICT DO NOTHING", [roomId, ctx.userId]);
  return JSON.stringify({
    ok: true,
    room_id: roomId,
    match_id: String(room.mid),
    mode: String(room.mode),
    map: String(room.map),
    members: parseInt(String(room.members), 10) + 1
  });
};
var rpcRoomLeave = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_room_leave", ctx.userId, "rate_asset_per_min", 30);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  const roomId = currentRoomId(nk, ctx.userId);
  if (!roomId) {
    return JSON.stringify({ ok: true, left: false });
  }
  const owner = nk.sqlQuery("SELECT owner_id::text AS oid FROM game.room WHERE room_id=$1::uuid", [roomId]);
  if (owner.length && String(owner[0].oid) === ctx.userId) {
    nk.sqlQuery("DELETE FROM game.room WHERE room_id=$1::uuid", [roomId]);
    logger.info("room dissolved by owner", "room", roomId);
    return JSON.stringify({ ok: true, left: true, dissolved: true });
  }
  nk.sqlQuery("DELETE FROM game.room_member WHERE room_id=$1::uuid AND user_id=$2::uuid", [roomId, ctx.userId]);
  return JSON.stringify({ ok: true, left: true, dissolved: false });
};
var rpcRoomList = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_room_list", ctx.userId, "rate_lobby_per_min", 120);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  const rows = nk.sqlQuery(
    "SELECT r.room_id::text AS rid, r.mode, r.map, r.max_members, u.username AS owner, (SELECT count(*) FROM game.room_member m WHERE m.room_id=r.room_id)::int AS members FROM game.room r JOIN users u ON u.id=r.owner_id ORDER BY r.created_at DESC LIMIT 50"
  );
  const rooms = [];
  for (let i = 0; i < rows.length; i++) {
    const r = rows[i];
    rooms.push({
      room_id: String(r.rid),
      mode: String(r.mode),
      map: String(r.map),
      owner: String(r.owner),
      members: parseInt(String(r.members), 10),
      max_members: parseInt(String(r.max_members), 10)
    });
  }
  return JSON.stringify({ ok: true, rooms });
};
var rpcRoomStart = function(ctx, logger, nk, params) {
  if (ctx.userId) {
    guardRate(nk, "rpc_room_start", ctx.userId, "rate_asset_per_min", 30);
  }
  if (!ctx.userId) {
    throw new Error("UNAUTHENTICATED");
  }
  parseInput(params);
  const roomId = currentRoomId(nk, ctx.userId);
  if (!roomId) {
    throw new Error("NOT_IN_ROOM");
  }
  const room = nk.sqlQuery("SELECT owner_id::text AS oid, mode FROM game.room WHERE room_id=$1::uuid", [roomId]);
  if (!room.length) {
    throw new Error("ROOM_NOT_FOUND");
  }
  if (String(room[0].oid) !== ctx.userId) {
    throw new Error("NOT_OWNER: \u4EC5\u623F\u4E3B\u53EF\u5F00\u5C40");
  }
  const mode = String(room[0].mode);
  const members = nk.sqlQuery(
    "SELECT user_id::text AS uid FROM game.room_member WHERE room_id=$1::uuid",
    [roomId]
  );
  if (members.length < 2) {
    throw new Error("NOT_ENOUGH_PLAYERS: \u81F3\u5C11 2 \u4EBA");
  }
  for (let i = 0; i < members.length; i++) {
    const uid = String(members[i].uid);
    const mmrRows = nk.sqlQuery("SELECT mmr FROM game.player_rating WHERE user_id=$1::uuid AND mode=$2", [uid, mode]);
    const mmr = mmrRows.length ? parseInt(String(mmrRows[0].mmr), 10) : 1e3;
    nk.sqlQuery(
      "INSERT INTO game.match_queue(user_id,mode,mmr,queued_at,match_id,join_token,party_id) VALUES ($1::uuid,$2,$3,now(),NULL,NULL,$4::uuid) ON CONFLICT (user_id) DO UPDATE SET mode=EXCLUDED.mode, mmr=EXCLUDED.mmr, queued_at=now(), match_id=NULL, join_token=NULL, party_id=EXCLUDED.party_id",
      [uid, mode, mmr, roomId]
    );
  }
  nk.sqlQuery("DELETE FROM game.room WHERE room_id=$1::uuid", [roomId]);
  ensureQueueProcessor(nk, logger);
  logger.info("room started", "room", roomId, "players", members.length, "mode", mode);
  return JSON.stringify({ ok: true, started: members.length, mode });
};
var zkLobbyInit = function(ctx, logger, nk, params) {
  return { state: { room_id: String(params.room_id || ""), presences: [], idle: 0 }, tickRate: 1, label: "zk_lobby" };
};
var zkLobbyJoinAttempt = function(ctx, logger, nk, dispatcher, tick, state, presence, metadata) {
  if (!state.room_id) {
    return { state, accept: false };
  }
  const rows = nk.sqlQuery(
    "SELECT 1 AS ok FROM game.room_member WHERE room_id=$1::uuid AND user_id=$2::uuid",
    [state.room_id, presence.userId]
  );
  return { state, accept: rows.length > 0 };
};
var zkLobbyJoin = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  for (let i = 0; i < presences.length; i++) {
    state.presences.push(presences[i]);
  }
  state.idle = 0;
  return { state };
};
var zkLobbyLeave = function(ctx, logger, nk, dispatcher, tick, state, presences) {
  for (let i = 0; i < presences.length; i++) {
    const p = presences[i];
    for (let j = state.presences.length - 1; j >= 0; j--) {
      if (state.presences[j].sessionId === p.sessionId) {
        state.presences.splice(j, 1);
      }
    }
  }
  return { state };
};
var zkLobbyLoop = function(ctx, logger, nk, dispatcher, tick, state, messages) {
  const exists = nk.sqlQuery("SELECT 1 AS ok FROM game.room WHERE room_id=$1::uuid", [state.room_id]);
  if (!exists.length) {
    return null;
  }
  for (let i = 0; i < messages.length; i++) {
    const m = messages[i];
    const op = Number(m.opCode);
    if (op >= 1 && op <= 9) {
      dispatcher.broadcastMessage(op, bytesToStr(m.data), null, null, true);
    }
  }
  if (state.presences.length === 0) {
    state.idle += 1;
    if (state.idle > 10) {
      return null;
    }
  } else {
    state.idle = 0;
  }
  return { state };
};
var zkLobbyTerminate = function(ctx, logger, nk, dispatcher, tick, graceSeconds) {
  return { room_id: "", presences: [], idle: 0 };
};
var zkLobbySignal = function(ctx, logger, nk, dispatcher, tick, state, data) {
  return { state, data };
};
var zkLobbyHandlers = {
  matchInit: zkLobbyInit,
  matchJoinAttempt: zkLobbyJoinAttempt,
  matchJoin: zkLobbyJoin,
  matchLeave: zkLobbyLeave,
  matchLoop: zkLobbyLoop,
  matchTerminate: zkLobbyTerminate,
  matchSignal: zkLobbySignal
};

// src/main.ts
var InitModule = function(ctx, logger, nk, initializer) {
  initializer.registerBeforeWriteStorageObjects(beforeWriteStorageObjects);
  initializer.registerBeforeDeleteStorageObjects(beforeDeleteStorageObjects);
  initializer.registerRpc("rpc_ping", rpcPing);
  initializer.registerRpc("rpc_shop_buy", rpcShopBuy);
  initializer.registerRpc("rpc_gacha_draw", rpcGachaDraw);
  initializer.registerRpc("rpc_item_enhance", rpcItemEnhance);
  initializer.registerRpc("rpc_loadout_set", rpcLoadoutSet);
  initializer.registerRpc("rpc_bootstrap_get", rpcBootstrapGet);
  initializer.registerRpc("rpc_catalog_get", rpcCatalogGet);
  initializer.registerRpc("rpc_gm_grant_currency", rpcGmGrantCurrency);
  initializer.registerRpc("rpc_match_create", rpcMatchCreate);
  initializer.registerRpc("rpc_match_settle", rpcMatchSettle);
  initializer.registerMatchmakerMatched(matchmakerMatched);
  initializer.registerMatch("zk_match", zkMatchHandlers);
  initializer.registerMatch("zk_queueproc", zkQueueProcHandlers);
  initializer.registerRpc("rpc_queue_enter", rpcQueueEnter);
  initializer.registerRpc("rpc_queue_cancel", rpcQueueCancel);
  initializer.registerRpc("rpc_queue_status", rpcQueueStatus);
  initializer.registerRpc("rpc_account_bind_email", rpcAccountBindEmail);
  initializer.registerRpc("rpc_lobby_channel_get", rpcLobbyChannelGet);
  initializer.registerRpc("rpc_lobby_heartbeat", rpcLobbyHeartbeat);
  initializer.registerRpc("rpc_online_list", rpcOnlineList);
  initializer.registerRpc("rpc_room_create", rpcRoomCreate);
  initializer.registerRpc("rpc_room_join", rpcRoomJoin);
  initializer.registerRpc("rpc_room_leave", rpcRoomLeave);
  initializer.registerRpc("rpc_room_list", rpcRoomList);
  initializer.registerRpc("rpc_room_start", rpcRoomStart);
  initializer.registerMatch("zk_lobby", zkLobbyHandlers);
  logger.info("ZombieKing modules loaded (P6-lobby)");
};
