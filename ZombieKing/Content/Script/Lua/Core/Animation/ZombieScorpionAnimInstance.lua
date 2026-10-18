-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Animation/ZombieScorpionAnimInstance.lua
-- 模块路径: Lua.Core.Animation.ZombieScorpionAnimInstance
--            （与 C++ UZombieScorpionAnimInstance::LuaScriptPath 保持一致）
--
-- 竞技生化网游：蝎子（多足爬墙角色）专属动画脚本（继承通用动画基类）
-- 适用范围：仅蝎子角色。多足体态各不相同（骨架/状态机无法跨体态复用），
--           其他体态（蜘蛛、蜈蚣等）各自建立专属动画脚本，互不继承。
-- 热更新职责：本体态步频/阈值调参、进出爬墙/蹬墙弹射的动画侧表现钩子。
-- =============================================================================

local ZombieScorpionAnimInstance = Class("Lua.Core.Animation.GameAnimInstanceBase")

-- =============================================================================
-- 初始化（继承链路：C++ → 本脚本 → 父脚本通用调参）
-- =============================================================================

function ZombieScorpionAnimInstance:OnAnimInstanceInitialized()
    -- 先执行通用调参（MovingSpeedThreshold / LocomotionReferenceSpeed）
    self.Super.OnAnimInstanceInitialized(self)

    -- ---------------------------------------------------------------------
    -- 蝎子专项热更新调参区
    -- ---------------------------------------------------------------------
    -- 说明：v1 版本爬墙复用爬行步态（ClimbBlendSpace 指向爬行混合空间），
    --       暂无爬墙专属参考速度；后续接入墙面专用动画后，
    --       在此下发爬墙步频/姿态缩放等参数。
end

-- =============================================================================
-- 运动模式变化表现（进出爬墙/腾空/落地的动画侧表现钩子）
-- =============================================================================

function ZombieScorpionAnimInstance:OnAnimMovementModeChanged(PreviousModeName, NewModeName)
    self.Super.OnAnimMovementModeChanged(self, PreviousModeName, NewModeName)

    -- 进入爬墙："Climbing" → 可在此做贴墙音效/镜头微调等表现；
    -- 蹬墙弹射："Climbing" → "Falling" → 可在此做弹射破空音效等表现。
    -- 注意：角色层面的爬墙表现（热成像/UI）已在 ZombieCrawlerCharacter.lua 的
    --       OnClimbStateChanged 中处理，本钩子只补充"动画实例视角"的表现需求。
end

return ZombieScorpionAnimInstance
