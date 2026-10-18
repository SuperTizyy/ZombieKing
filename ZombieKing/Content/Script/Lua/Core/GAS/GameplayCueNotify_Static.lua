-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/GAS/GameplayCueNotify_Static.lua
-- 模块路径: Lua.Core.GAS.GameplayCueNotify_Static
--            （由 C++ 的 UGameGameplayCueNotify_Static 通过 IUnLuaInterface 绑定）
--
-- 竞技生化网游：静态 GameplayCue（一次性爆发表现）的默认 Lua 入口
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（UGameGameplayCueNotify_Static）：只做"机制" —— 绑定本脚本、把引擎的
--     Cue 分发（Executed / OnActive / WhileActive / Removed）收敛为一个统一事件；
--   * Lua 层（本文件）：全部"表现" —— 生成粒子、播音效、镜头反馈、飘字等。
--     放在这里的核心收益：线上可不停机热更、调参无需重编译 C++。
--
-- 说明：本文件是**默认共享实现**（只打日志）。
--       实际项目应为每个 Cue 通知资产单独指定 LuaScriptPath（如
--       Lua.Core.GAS.Cues.GC_Damage_Head），在各自的脚本里实现 OnGameplayCue。
--       若资产未指定 LuaScriptPath，则回落到本脚本（保证不会因缺脚本而报错）。
--
-- 常用 Cue 参数（Parameters，均为引擎结构）：
--   Parameters.Location / Parameters.Normal     -- 表现位置与法线
--   Parameters.Instigator                       -- 伤害发起者（可为空）
--   Parameters.EffectCauser                     -- 效果来源（一般为武器/技能持有者）
--   Parameters.EffectContext                    -- 效果上下文（可从中取命中信息，如命中骨骼）
--
-- 相关 Cue 标签见 Source/ZombieKing/GAS/GameplayCueTags.h
-- （资产名 → 标签的自动推导规则也在该文件头部注释里）。
-- =============================================================================

local GameplayCueNotify_Static = Class()

-- Cue 分发统一入口：由 C++ 在收到引擎 Cue 事件时派发
-- @param MyTarget    Cue 作用的目标 Actor（通常是被命中 / 死亡 / 变异的角色）
-- @param EventType   引擎事件类型（GameplayCueEvent：Executed / OnActive / WhileActive / Removed）
-- @param Parameters  引擎 Cue 参数（位置、法线、来源、效果上下文等）
function GameplayCueNotify_Static:OnGameplayCue(MyTarget, EventType, Parameters)
    -- 默认占位：仅打印一次，便于在无内容资产时确认"通道已打通"
    print(string.format("[Cue][Static] target=%s event=%s", tostring(MyTarget), tostring(EventType)))
end

return GameplayCueNotify_Static
