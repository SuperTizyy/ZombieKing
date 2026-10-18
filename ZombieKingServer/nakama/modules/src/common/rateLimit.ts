// RPC 频控限流（架构文档承诺项）：PG 原子固定窗口计数。
// goja 多 worker runtime 内存不共享，进程内 Map 会漏计，故用单条 UPSERT SQL 保证准确。

export function rateCfg(nk: nkruntime.Nakama, key: string, def: number): number {
  const rows = nk.sqlQuery("SELECT value FROM game.sys_config WHERE key=$1", [key]);
  const v = rows.length ? parseInt(String(rows[0].value), 10) : def;
  return isNaN(v) ? def : v;
}

/** 返回 true=放行；超限由调用方 throw RATE_LIMITED */
export function checkRate(nk: nkruntime.Nakama, rpcId: string, userId: string, limitPerMin: number): boolean {
  if (limitPerMin <= 0) { return true; }
  const rows = nk.sqlQuery(
    "INSERT INTO game.rate_limit(key, window_start, count) VALUES ($1, date_trunc('minute', now()), 1) " +
    "ON CONFLICT (key, window_start) DO UPDATE SET count = game.rate_limit.count + 1 RETURNING count",
    [rpcId + ":" + userId]);
  if (!rows.length) { return true; }
  return parseInt(String(rows[0].count), 10) <= limitPerMin;
}

/** 组合：读取配置 + 检查，超限直接抛错 */
export function guardRate(nk: nkruntime.Nakama, rpcId: string, userId: string, cfgKey: string, def: number): void {
  if (!checkRate(nk, rpcId, userId, rateCfg(nk, cfgKey, def))) {
    throw new Error("RATE_LIMITED: 操作过于频繁，请稍后再试");
  }
}
