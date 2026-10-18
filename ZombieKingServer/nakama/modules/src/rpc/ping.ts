// P1 冒烟 RPC：验证 TS 模块加载 + 运行时到 PostgreSQL 的查询通路

export const rpcPing: nkruntime.RpcFunction = function (ctx, logger, nk, params) {
  const rows = nk.sqlQuery("SELECT 1");
  return JSON.stringify({
    ok: true,
    module: "zombieking-p2",
    dbReachable: rows.length === 1,
    userId: ctx.userId || null,
  });
};
