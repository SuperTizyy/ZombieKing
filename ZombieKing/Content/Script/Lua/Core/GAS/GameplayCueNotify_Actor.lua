-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/GAS/GameplayCueNotify_Actor.lua
-- 模块路径: Lua.Core.GAS.GameplayCueNotify_Actor
--            （由 C++ 的 AGameGameplayCueNotify_Actor 通过 IUnLuaInterface 绑定）
--
-- 竞技生化网游：角色式 GameplayCue（有生命周期的持续表现）的默认 Lua 入口
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（AGameGameplayCueNotify_Actor）：只做"机制" —— 绑定本脚本、把引擎 Cue 分发
--     收敛为一个统一事件，并保留引擎的"激活 / 复用 / 结束"状态机；
--   * Lua 层（本文件）：全部"表现" —— 持续特效的生成、跟随、停止与清理。
--
-- Cue 生命周期（引擎事件，均由本函数统一接收）：
--   OnActive    —— 首次激活：开始播放持续表现（本端亲眼看到激活时才触发）
--   WhileActive —— 已处于激活状态（含"中途加入"者补看）：只补表现、不要重复初始化逻辑
--   Removed     —— 移除：停止并清理表现（注意：引擎随后可能回收甚至销毁本 Cue Actor，
--                  因此清理务必在本次回调内同步完成）
--
-- 说明：本文件是**默认共享实现**（只打日志）。
--       实际项目应为每个持续 Cue 资产单独指定 LuaScriptPath，在其脚本里实现 OnGameplayCue。
--
-- 相关 Cue 标签见 Source/ZombieKing/GAS/GameplayCueTags.h。
-- =============================================================================

local GameplayCueNotify_Actor = Class()

-- Cue 分发统一入口：由 C++ 在收到引擎 Cue 事件时派发（先于引擎状态机执行）
-- @param MyTarget    Cue 作用的目标 Actor
-- @param EventType   引擎事件类型（GameplayCueEvent：OnActive / WhileActive / Removed）
-- @param Parameters  引擎 Cue 参数（位置、法线、来源、效果上下文等）
function GameplayCueNotify_Actor:OnGameplayCue(MyTarget, EventType, Parameters)
    -- 默认占位：仅打印一次，便于在无内容资产时确认"通道已打通"
    print(string.format("[Cue][Actor] target=%s event=%s", tostring(MyTarget), tostring(EventType)))
end

return GameplayCueNotify_Actor
