-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Bootstrap.lua
-- 模块路径: Lua.Core.Bootstrap
--            （在 Config/DefaultUnLuaSettings.ini 的 StartupModuleName 中配置为启动入口）
--
-- UnLua 环境启动入口（只做"调试环境引导"，严禁在此写业务逻辑）
-- 命名说明：本文件刻意不叫 GameInstance —— 它是"UnLua 启动入口"，与框架层的
--           GameInstance 脚本（Lua.Core.GameFramework.GameInstance，由 UGameGameInstance 绑定）
--           职责完全不同，分开命名以免二者混淆。
-- 职责：只做调试环境准备；业务逻辑应挂在各自的 Actor / UI / GameInstance 脚本中，
--       由 UnLua 的绑定机制按需加载。
-- =============================================================================

-- 引导 LuaPanda 调试环境（预加载 lua54.dll、追加 C 模块搜索路径、attach 调试器）
-- 详见 DebugBootstrap.lua 头部的踩坑记录；发布构建可通过去掉本行关闭调试引导。
require("Lua.Core.DebugBootstrap")

print("[UnLua] 启动入口 Lua.Core.Bootstrap 已执行")
