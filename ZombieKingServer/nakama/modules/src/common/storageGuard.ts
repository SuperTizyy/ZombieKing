// 安全红线 R1：客户端禁止直接写入数据库（架构文档 v1.0 §4.3.2）
// 除 settings 集合（限大小）外，一律拒绝客户端写/删 Storage。
//
// v3.41.0 实测钩子约定：(ctx, logger, nk, input)，input 为包装对象
// （写：{objects:[...]}，删：{objectIds:[...]}）；throw 即拒绝，返回 undefined 放行。

const SETTINGS_COLLECTION = "settings";
const SETTINGS_MAX_BYTES = 8192;

export const beforeWriteStorageObjects: nkruntime.BeforeWriteStorageObjectsFunction = function (
  ctx, logger, nk, input
) {
  const objects = (input && input.objects) ? input.objects : [];
  for (let i = 0; i < objects.length; i++) {
    const o = objects[i];
    if (o.collection !== SETTINGS_COLLECTION) {
      logger.warn("拒绝客户端写 Storage", "userId", ctx.userId, "collection", o.collection);
      throw new Error("客户端禁止写入该集合: " + o.collection);
    }
    const raw = typeof o.value === "string" ? o.value : JSON.stringify(o.value || {});
    if (raw.length > SETTINGS_MAX_BYTES) {
      logger.warn("拒绝客户端写 settings 超限", "userId", ctx.userId, "bytes", raw.length);
      throw new Error("settings 超出大小限制: " + raw.length + " bytes");
    }
  }
  return input; // 原样返回 = 放行 settings（返回 null 会被视为中止并产生 NOT_FOUND）
};

export const beforeDeleteStorageObjects: nkruntime.BeforeDeleteStorageObjectsFunction = function (
  ctx, logger, nk, input
) {
  logger.warn("拒绝客户端删除 Storage", "userId", ctx.userId);
  throw new Error("客户端禁止删除 Storage");
};
