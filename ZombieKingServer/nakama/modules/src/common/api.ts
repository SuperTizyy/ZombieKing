// RPC 公共工具：入参解析、幂等键校验、fn_* 存储过程单次调用、服务端熵源掷点

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function parseInput(params: string): any {
  if (!params) { return {}; }
  try {
    return JSON.parse(params);
  } catch (e) {
    throw new Error("INVALID_INPUT: params 必须是 JSON");
  }
}

export function requireIdem(input: any): string {
  const k = input.idempotency_key;
  if (typeof k !== "string" || !UUID_RE.test(k)) {
    throw new Error("INVALID_INPUT: 缺少 idempotency_key(UUID)");
  }
  return k;
}

export function requireUuid(v: any, name: string): string {
  if (typeof v !== "string" || !UUID_RE.test(v)) {
    throw new Error("INVALID_INPUT: " + name + " 必须是 UUID");
  }
  return v;
}

/** goja 把 match 消息 data/bytes 传成字节数组或 ArrayBuffer，解码为字符串 */
export function bytesToStr(v: any): string {
  if (typeof v === "string") { return v; }
  if (v === null || v === undefined) { return ""; }
  let a = v;
  if (typeof a.length !== "number" && typeof a.byteLength === "number") {
    a = new Uint8Array(a);
  }
  let s = "";
  for (let i = 0; i < a.length; i++) { s += String.fromCharCode(a[i]); }
  return s;
}

/** goja 把 PG jsonb/bytea 传成字节数组或字符串，统一解码为对象 */
export function decodeJsonb(v: any): any {
  if (v === null || v === undefined) { return null; }
  if (typeof v === "string") { return JSON.parse(v); }
  if (typeof v === "object" && typeof v.length === "number") {
    let s = "";
    for (let i = 0; i < v.length; i++) { s += String.fromCharCode(v[i]); }
    return JSON.parse(s);
  }
  return v; // 已是对象
}

/** 单次 nk.sqlQuery 调用 PL/pgSQL 函数（函数体内单事务，见项目规则事务铁律） */
export function callFn(nk: nkruntime.Nakama, name: string, args: any[]): any {
  const holders: string[] = [];
  for (let i = 0; i < args.length; i++) { holders.push("$" + (i + 1)); }
  const rows = nk.sqlQuery("SELECT game." + name + "(" + holders.join(",") + ") AS res", args);
  if (!rows || rows.length === 0) { throw new Error("FN_NO_RESULT: " + name); }
  return decodeJsonb(rows[0].res);
}

export function failIfErr(res: any): any {
  if (res && res.err) { throw new Error(res.err); }
  return res;
}

/** 服务端权威随机：以 nk.uuidv4() 为熵源映射到 [0,1)，禁止客户端种子 */
export function rollFloat(nk: nkruntime.Nakama): number {
  const hex = nk.uuidv4().replace(/-/g, "");
  return parseInt(hex.substring(0, 8), 16) / 0xFFFFFFFF;
}

/** 按权重表抽取一项 */
export function weightedPick<T extends { weight: number }>(nk: nkruntime.Nakama, items: T[]): T {
  let total = 0;
  for (let i = 0; i < items.length; i++) { total += items[i].weight; }
  let r = rollFloat(nk) * total;
  for (let i = 0; i < items.length; i++) {
    r -= items[i].weight;
    if (r < 0) { return items[i]; }
  }
  return items[items.length - 1];
}

export function parseJson<T>(v: any, fallback: T): T {
  if (v === null || v === undefined || v === "") { return fallback; }
  const decoded = decodeJsonb(v);
  if (decoded !== null && typeof decoded === "object") { return decoded as T; }
  try { return JSON.parse(String(v)) as T; } catch (e) { return fallback; }
}
