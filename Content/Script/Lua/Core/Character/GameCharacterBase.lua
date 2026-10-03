-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Character/GameCharacterBase.lua
-- 模块路径: Lua.Core.Character.GameCharacterBase
--            （UnLua 脚本根目录 = Content/Script，模块名按目录层级用 . 连接）
--
-- 竞技生化网游：通用角色基类脚本（双足人类 / 多足生化幽灵共用）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层：移动/网络/GAS 权威逻辑，只向 Lua 派发"原始事件"；
--   * Lua 层（本文件及子类）：表现（镜头/特效/音效/UI）与技能激活请求。
-- =============================================================================

local GameCharacterBase = Class()

-- =============================================================================
-- 生命周期
-- =============================================================================

function GameCharacterBase:ReceiveBeginPlay()
    -- 判断是否为本地客户端驱动
    if self:IsLocallyControlled() then
        print("[UnLua] 本地真第一人称角色就绪:", self:GetName())
        self:InitLocalHUD()
    end
end

-- =============================================================================
-- 输入事件契约（由 C++ 基类的 BlueprintImplementableEvent 派发，子类按需重写）
-- =============================================================================

-- 移动输入（ForwardAxis/RightAxis 为 -1..1 原始轴值，仅表现层使用）
function GameCharacterBase:OnInputMove(ForwardAxis, RightAxis)
    -- 子类重写：步足/腿部动画混合参数、运动状态切换表现等
end

-- 视角输入（YawAxis/PitchAxis 为原始轴值，仅表现层使用）
function GameCharacterBase:OnInputLook(YawAxis, PitchAxis)
    -- 子类重写：热成像视野微动、镜头惯性表现等
end

-- 攻击输入（Lua 中发起 GAS 技能激活请求 + 第一人称攻击表现）
function GameCharacterBase:OnInputAttack()
    -- 子类重写
end

-- 跳跃输入（本地跳跃表现；权威位移由 C++ Mover 输入指令处理）
function GameCharacterBase:OnInputJump()
    -- 子类重写
end

-- 进出爬墙状态变化（多足角色专用，人类角色不会收到）
function GameCharacterBase:OnClimbStateChanged(bEnterClimb)
    -- 子类重写
end

-- =============================================================================
-- 受击表现（由 C++ CombatComponent 的受击委托派发进 Lua，当前为空壳预留）
-- =============================================================================

-- 响应受击事件
function GameCharacterBase:OnDamaged_Implementation(DamageAmount, HitBone, InstigatorActor)
    if self:IsLocallyControlled() then
        -- 本地第一人称屏幕受击红光、准星受击抖动
        self:PlayCameraShake(DamageAmount)
        self:UpdateHitIndicator(InstigatorActor)
    else
        -- 远端玩家看到的受击血液火花特效
        self:SpawnBloodImpactVFX(HitBone)
    end
end

-- =============================================================================
-- 表现空实现（占位，避免受击链路在具体表现未接入前出现 nil 调用报错）
-- =============================================================================

-- 本地镜头受击抖动
function GameCharacterBase:PlayCameraShake(DamageAmount)
    -- TODO: 接入 UCameraShakeBase（受击强度按 DamageAmount 分级振幅）
end

-- 受击方向指示器（屏幕边缘红色扇形指示伤害来源）
function GameCharacterBase:UpdateHitIndicator(InstigatorActor)
    -- TODO: 计算 InstigatorActor 相对玩家视角的方位，驱动 HUD 受击指示
end

-- 远端受击血液/火花特效
function GameCharacterBase:SpawnBloodImpactVFX(HitBone)
    -- TODO: 按命中骨骼位置生成血液 Niagara 特效与贴花
end

-- 本地 HUD 初始化
function GameCharacterBase:InitLocalHUD()
    -- 通知 UI 模块绑定 PlayerState 中的生命值与子弹计数
end

return GameCharacterBase
