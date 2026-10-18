-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/UI/GameUserWidgetBase.lua
-- 模块路径: Lua.Core.UI.GameUserWidgetBase
--            （由 C++ 的 UGameUserWidgetBase 通过 IUnLuaInterface 绑定）
--
-- 竞技生化网游：前端界面（主菜单 / 设置 / 房间等）的默认 Lua 入口
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（UGameUserWidgetBase）：只做"机制" —— 绑定本脚本、把 Slate 生命周期
--     收敛为 OnWidgetShow / OnWidgetHide 两个事件、并提供"我在哪个路由/哪一层"的只读信息；
--   * Lua 层（本文件）：全部"表现与交互" —— 按钮响应、数据拉取与刷新、动画与音效等。
--
-- 说明：本文件是**默认共享实现**（只打日志）。
--       实际项目应为每个界面单独指定 LuaScriptPath（如 Lua.Core.UI.MainMenu），
--       在其脚本里实现 OnWidgetShow / OnWidgetHide；未指定时回落到本脚本（保证不因缺脚本而报错）。
--
-- 常用 C++ 契约（均可直接调用）：
--   self:GetRoute()        -- 本界面的路由名（FName）
--   self:GetUILayer()      -- 本界面所处层级（EGameUILayer）
--   self:GetUISubsystem()  -- UI 管理层：用它关闭自己 / 推入其它界面
--        :PushRoute(RouteName)          -- 推入界面（如 "Settings"）
--        :PopRouteOnLayer(Layer)        -- 弹出某层栈顶
--        :CloseRoute(RouteName)         -- 关闭指定界面
--        :CloseAllRoutes()              -- 清空界面栈
--   self:GetOwningPlayer() -- 本机玩家控制器（取 PlayerState / GameState 的入口）
--
-- 取服务（会话 / 遥测 / 流程）：
--   UE.UGameplayStatics.GetGameInstance(self):GetSubsystem(UE.UGameSessionSubsystem)
--   UE.UGameplayStatics.GetGameInstance(self):GetSubsystem(UE.UGameFlowSubsystem)
-- =============================================================================

local GameUserWidgetBase = Class()

-- 界面显示（已加入视口）：初始化显示、拉取数据、播放入场动画写在这里
function GameUserWidgetBase:OnWidgetShow()
    print(string.format("[UI] 界面显示：%s", tostring(self:GetRoute())))
end

-- 界面隐藏（即将移出视口）：停止动画、解绑临时委托写在这里
function GameUserWidgetBase:OnWidgetHide()
    print(string.format("[UI] 界面隐藏：%s", tostring(self:GetRoute())))
end

return GameUserWidgetBase
