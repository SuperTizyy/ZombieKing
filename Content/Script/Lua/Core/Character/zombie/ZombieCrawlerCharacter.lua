-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Character/zombie/ZombieCrawlerCharacter.lua
-- 模块路径: Lua.Core.Character.zombie.ZombieCrawlerCharacter
--            （与 C++ AZombieCrawlerCharacter::LuaScriptPath 保持一致）
--
-- 竞技生化模式：多足爬墙幽灵角色表现层
-- 职责划分：
--   * C++ 层（AZombieCrawlerCharacter / UCrawlerMoverComponent / UCrawlerClimbMode）：
--     移动、吸附判定、蹬墙弹射等全部权威逻辑（位于 Mover 预测管线内）；
--   * Lua 层（本文件）：热成像视界、爬墙状态演出、攻击技能请求等纯表现与请求逻辑。
-- =============================================================================

local ZombieCrawlerCharacter = Class("Lua.Core.Character.GameCharacterBase")

-- =============================================================================
-- 生命周期
-- =============================================================================

function ZombieCrawlerCharacter:ReceiveBeginPlay()
    -- 调用父类初始化（处理真第一人称相机裁剪与 HUD 基础绑定）
    self.Super.ReceiveBeginPlay(self)

    if self:IsLocallyControlled() then
        print("[UnLua] 成功加载多足生化角色控制逻辑:", self:GetName())
        self:InitCrawlerView()
    end
end

-- 初始化多足生化幽灵的第一人称特殊视界（红外夜视/热成像效果）
function ZombieCrawlerCharacter:InitCrawlerView()
    -- 1. 生化幽灵通常配置更大的广角视野，增强贴脸压迫感（相机组件由 C++ 暴露给 Lua 访问）
    -- local Camera = self.FirstPersonCamera
    -- if Camera then
    --     Camera:SetFieldOfView(105.0)
    -- end

    -- 2. 激活屏幕全屏后处理材质（红外扫描、人类绿色高亮透视）
    -- TODO: 添加 PostProcess 材质到本地相机的 PostProcessSettings
    print("[UnLua] 激活生化幽灵热成像后处理滤镜")
end

-- =============================================================================
-- 输入事件（由 C++ 基类 BlueprintImplementableEvent 派发）
-- =============================================================================

-- 重写攻击输入（响应 C++ 派发的 OnInputAttack）
function ZombieCrawlerCharacter:OnInputAttack()
    if not self:IsLocallyControlled() then
        return
    end

    print("[UnLua] 多足爬虫发动强力爪击")

    -- 1. 播放第一人称双前肢挥击蒙太奇（Montage）
    -- self:PlayAnimMontage(self.ClawAttackMontage)

    -- 2. 触发第一人称攻击镜头微抖（Camera Shake）
    -- 3. 发送 GAS 技能激活请求给服务端的 PlayerState ASC（技能在服务器结算，客户端只发起请求）
    local ASC = self:GetAbilitySystemComponent()
    if ASC then
        -- 尝试激活技能 Tag：Ability.Skill.ZombieClaw
        -- ASC:TryActivateAbilitiesByTag(...)
        -- TODO: 技能体系接入后启用（当前 GamePlayerState 为空壳占位）
    end
end

-- 响应跳跃输入（地面跳跃/爬墙蹬墙弹射共用一个按键）
function ZombieCrawlerCharacter:OnInputJump()
    if not self:IsLocallyControlled() then
        return
    end

    if self:IsClimbing() then
        -- 爬墙中跳跃 = 蹬墙弹射脱离（权威位移由 C++ UCrawlerClimbMode 在模拟内完成）
        print("[UnLua] 多足爬虫执行表面蹬墙跳跃")
        -- TODO: 播放蹬墙弹射音效与腿部张开动画（可监听 OnClimbStateChanged(false) 统一处理脱离表现）
    else
        print("[UnLua] 多足爬虫执行地面跳跃")
        -- TODO: 地面起跳音效/蓄力表现
    end
end

-- 响应爬墙状态变化（C++ HandleMovementModeChanged 委托经基类事件派发）
-- bEnterClimb = true 进入爬墙，false 脱离爬墙
function ZombieCrawlerCharacter:OnClimbStateChanged(bEnterClimb)
    if not self:IsLocallyControlled() then
        -- 远端玩家眼中：爪子抓墙粒子/音效（所有端都需要表现）
        -- TODO: 按 bEnterClimb 播放吸附/脱离的音频与 Niagara 特效
        return
    end

    if bEnterClimb then
        print("[UnLua] 进入爬墙模式：切换镜头 FOV / 增强热成像强度")
        -- TODO: 镜头 FOV 拉近 + 屏幕四周后处理压暗，强化贴墙压迫感
    else
        print("[UnLua] 脱离爬墙模式：恢复常规视界")
        -- TODO: 恢复默认 FOV 与后处理参数
    end
end

-- 移动输入表现钩子（原始轴值；权威移动由 C++ Mover 输入指令处理）
function ZombieCrawlerCharacter:OnInputMove(ForwardAxis, RightAxis)
    -- TODO: 驱动步足动画混合（前进/横移/贴墙爬行三套步伐参数）
end

-- 视角输入表现钩子
function ZombieCrawlerCharacter:OnInputLook(YawAxis, PitchAxis)
    -- TODO: 热成像扫描线随视角轻微偏移的镜头惯性表现
end

return ZombieCrawlerCharacter
