-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/GameInstance.lua
-- 模块路径: Lua.Core.GameInstance
--            （在 Config/DefaultUnLuaSettings.ini 的 StartupModuleName 中配置为启动入口）
--
-- UnLua 环境启动入口（开发期调试引导）
-- 职责：只做"调试环境准备"，严禁在此写业务逻辑。
-- 业务逻辑应挂在各自的 Actor/UI 脚本中，由 UnLua 绑定机制按需加载。
-- =============================================================================

-- 引导 LuaPanda 调试环境（预加载 lua54.dll、追加 C 模块搜索路径、attach 调试器）
-- 详见 DebugBootstrap.lua 头部的踩坑记录；发布构建可通过去掉本行关闭调试引导。
require("Lua.Core.DebugBootstrap")

print("[UnLua] GameInstance 启动入口已执行")
