-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Animation/GameAnimInstanceBase.lua
-- 模块路径: Lua.Core.Animation.GameAnimInstanceBase
--            （与 C++ UGameAnimInstanceBase::LuaScriptPath 保持一致）
--
-- 竞技生化网游：通用动画实例脚本基类（所有角色的 AnimInstance 共用）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（UGameAnimInstanceBase）：每帧从 Mover 同步状态提取运动数据的
--     引擎桥接（速度/方向/落地/模式名），性能红线路径，只派发事件不写表现；
--   * Lua 层（本文件及子类）：可热更新的功能全部在此——
--     调参（停车阈值/播放速率参考速度）与模式变化表现（音效/特效/镜头），
--     改完即生效、无需重新打包资源；上线后可走 Lua 热补丁下发。
-- =============================================================================

local GameAnimInstanceBase = Class()

-- =============================================================================
-- 初始化（由 C++ NativeInitializeAnimation 末尾派发，每个动画实例仅一次）
-- =============================================================================

function GameAnimInstanceBase:OnAnimInstanceInitialized()
    -- ---------------------------------------------------------------------
    -- 热更新调参区（此处赋值覆盖 C++ 兜底默认值）
    -- ---------------------------------------------------------------------

    -- 判定"进入移动状态"的速度阈值（cm/s）：
    -- 调低 → 极慢速挪动也会切移动动画；调高 → 微抖动时保持待机姿态
    self.MovingSpeedThreshold = 10.0

    -- 动画 1.0x 播放速率对应的参考速度（cm/s）：用于消除步足滑步。
    -- 调参方法：与角色 Mover 设置（CommonLegacyMovementSettings.MaxSpeed）对照，
    -- 实际播放速率 = clamp(实际速度 / 本参考速度, 0.5, 1.5)（见父动画蓝图 Crawl 状态）
    self.LocomotionReferenceSpeed = 300.0
end

-- =============================================================================
-- 运动模式变化表现（由 C++ 每帧检测 Mover 模式名边沿变化后派发，仅变化时触发）
-- =============================================================================

-- PreviousModeName / NewModeName 为 Mover 模式名（如 "Walking" / "Falling" / "Climbing"）
function GameAnimInstanceBase:OnAnimMovementModeChanged(PreviousModeName, NewModeName)
    -- 子类重写：模式切换的表现（起跳/落地音效、姿态过渡特效、镜头反馈等）。
    -- 注意：此处只做表现；权威位移逻辑全部在 C++ 运动模拟管线内。
end

return GameAnimInstanceBase
