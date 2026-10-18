-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Character/GameCharacterBase.lua
-- 模块路径: Lua.Core.Character.GameCharacterBase
--            （UnLua 脚本根目录 = Content/Script，模块名按目录层级用 . 连接）
--
-- 竞技生化网游：通用角色基类脚本（双足人类 / 多足生化幽灵共用）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层：移动/网络/GAS 权威逻辑 + 武器机制（射速节流/弹药/换弹/切枪），只向 Lua 派发"原始事件"；
--   * Lua 层（本文件及子类）：表现（镜头/特效/音效/UI）与操作请求（开火/换弹/切枪）。
--
-- 武器系统分工（2026 武器系统里程碑）：
--   * C++（AGameCharacterBase + UGameWeaponComponent）：只做"按键 → 事件"的转发与权威机制；
--   * Lua（本文件）：开火/换弹/切枪的**操作请求**与**全部表现**
--     （后坐力、准星扩散、枪口火焰、音效、换弹动画、弹药 HUD）。
--   数值（射速/伤害/弹匣/散布…）全在 UGameWeaponDefinition 资产里；改枪感不必动 C++、也不必动 Lua。
-- =============================================================================

-- 音频入口（玩法只"报事件标签"，具体用哪个声音由音频事件表决定 —— 见 Lua.Core.Audio.Audio 头注释）
local Audio = require("Lua.Core.Audio.Audio")

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

    -- 武器组件事件订阅（各端都要订阅：本地玩家看自己与别人的开火表现，远端玩家看别人的开火表现）
    -- UnLua 约定：Add(持有回调的对象, 回调函数)，此处 owner 传角色自身（self 是带 UnLua 绑定的 UObject）
    local WeaponComponent = self:GetWeaponComponent()
    if WeaponComponent then
        WeaponComponent.OnFired:Add(self, self.OnWeaponFired)
        WeaponComponent.OnEmptyTriggered:Add(self, self.OnWeaponEmptyTriggered)
        WeaponComponent.OnAmmoChanged:Add(self, self.OnWeaponAmmoChanged)
        WeaponComponent.OnReloadStateChanged:Add(self, self.OnWeaponReloadStateChanged)
        WeaponComponent.OnWeaponChanged:Add(self, self.OnWeaponChanged)
    end

    -- 战斗反馈事件订阅（都由 C++ 服务器权威确认后才广播）：
    --   * OnHitConfirmed / OnKillConfirmed —— **只有攻击者本端**会收到（定向 Client RPC）；
    --   * OnStaggerChanged —— 受击硬直（各端按复制状态各自起倒计时）。
    self.OnHitConfirmed:Add(self, self.OnLocalHitConfirmed)
    self.OnKillConfirmed:Add(self, self.OnLocalKillConfirmed)
    self.OnStaggerChanged:Add(self, self.OnStaggered)
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

-- 攻击输入（"通用攻击"通道：近战爪击等不依赖武器的攻击）
-- 说明：权威判定与伤害全在服务器（见 UGameMeleeAttackAbility），
--       因此这里播放的蒙太奇/音效只是"即时反馈"，不参与任何命中判定。
function GameCharacterBase:OnInputAttack()
    -- 子类重写：播放挥击的第一人称蒙太奇、音效
end

-- 开火输入（按下）：把"开火请求"交给武器组件
-- 说明：射速节流、弹药扣减、命中结算全在服务器（组件内部权威裁决），
--       本函数只是"按下开火键"的意愿表达；连发节奏由组件的本地定时器控制（数值取自武器定义）。
function GameCharacterBase:OnInputFireStart()
    local WeaponComponent = self:GetWeaponComponent()
    if WeaponComponent == nil then
        return
    end
    WeaponComponent:StartFire()
end

-- 开火输入（松开）：停火（停止连发循环）
function GameCharacterBase:OnInputFireRelease()
    local WeaponComponent = self:GetWeaponComponent()
    if WeaponComponent == nil then
        return
    end
    WeaponComponent:StopFire()
end

-- 换弹输入：请求换弹
-- 说明：组件内部会做服务器权威校验（弹匣已满/无备弹/已在换弹都会被拒绝）；
--       如需"爬墙中禁止换弹"之类的附加规则，在本函数里加判断即可（规则留在 Lua，可热更）。
function GameCharacterBase:OnInputReload()
    local WeaponComponent = self:GetWeaponComponent()
    if WeaponComponent == nil then
        return
    end
    WeaponComponent:RequestReload()
end

-- 切枪输入：切到下一把
function GameCharacterBase:OnInputSwitchWeapon()
    local WeaponComponent = self:GetWeaponComponent()
    if WeaponComponent == nil then
        return
    end
    WeaponComponent:RequestNextWeapon()
end

-- 技能输入按下（通用技能通道；开火走 OnInputFireStart，不在此列）
function GameCharacterBase:OnInputAbility(InputTag)
    -- 子类重写：按键反馈、音效、准星状态变化等
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
-- 武器事件契约（由 C++ UGameWeaponComponent 的委托派发进 Lua）
-- =============================================================================

-- 已开火：播放开火表现
-- 触发来源有两处（都要能正确表现，且不会重复）：
--   ① 开火者本端：由武器组件"本地预测"在按下/连发时立即触发（不等服务器回包）；
--   ② 其他玩家的客户端：由服务器不可靠多播触发（同一个 Pawn 在本端不是"本地控制"时才播）。
function GameCharacterBase:OnWeaponFired(Weapon)
    if self:IsLocallyControlled() then
        self:PlayFirstPersonFireFeedback(Weapon)
    else
        self:PlayThirdPersonFireFeedback(Weapon)
    end
end

-- 空仓扣扳机（本地预判，不涉及权威结算）：播"枪机空击"声与 HUD 弹匣警示
function GameCharacterBase:OnWeaponEmptyTriggered()
    if self:IsLocallyControlled() then
        self:PlayEmptyClipFeedback()
    end
end

-- 弹药变化（扣弹、换弹完成）：刷新 HUD 弹药显示
function GameCharacterBase:OnWeaponAmmoChanged(Ammo, ReserveAmmo)
    if self:IsLocallyControlled() then
        self:UpdateAmmoHUD(Ammo, ReserveAmmo)
    end
end

-- 换弹状态变化：播放/停止换弹动画，驱动换弹进度条
function GameCharacterBase:OnWeaponReloadStateChanged(bReloading, ReloadSeconds)
    if bReloading then
        -- 换弹音：挂在武器挂点上（卸弹匣/拉枪机的声音来自枪身，而非枪口）
        local WeaponComponent = self:GetWeaponComponent()
        local Weapon = WeaponComponent and WeaponComponent:GetCurrentWeapon()
        if Weapon then
            Audio.PlayAttached(self, Weapon.ReloadSoundEventTag, self.CharacterMesh, Weapon.WeaponMeshAttachSocket)
        end

        -- TODO（表现）：播放换弹蒙太奇（按 ReloadSeconds 换算播放速率）、显示进度环
    else
        -- TODO（表现）：结束换弹动画与进度显示
    end
end

-- 当前武器变化（切枪）：刷新第一人称枪械模型与动画层、准星形态
function GameCharacterBase:OnWeaponChanged(SlotIndex, Weapon)
    -- TODO（表现）：按武器定义的 FirstPersonWeaponMesh / WeaponMeshAttachSocket 换枪模型，
    --               切换对应的动画层与准星配置
end

-- =============================================================================
-- 战斗反馈契约（受击硬直 / 命中确认 / 击杀确认）
-- =============================================================================

-- 受击硬直（本端广播）：播放硬直表现
-- 说明：硬直对**移动的限制**由 C++ 在输入侧完成（硬直期间不产生移动意图，见 GameCharacterBase.cpp 的 OnProduceInput），
--       因此 Lua 这边只负责"看得出被打中并僵了一下"的表现。
function GameCharacterBase:OnStaggered(DurationSeconds, InstigatorActor)
    if self:IsLocallyControlled() then
        -- 本地玩家：镜头受击抖动（强度可按时长缩放）+ 准星受击反馈
        self:PlayCameraShake(DurationSeconds)
    end

    -- TODO（表现）：按 DurationSeconds 播放硬直动画（上半身抖动 / 腿部踉跄），
    --               并触发受击音效（音频事件标签可由内容侧定义，如 Audio.Combat.Staggered）
end

-- 命中确认（**只有攻击者本端**收到）：HUD 命中标记 + 命中音效
-- 重要：这是"服务器确认我打中了"的权威反馈，不能用自己本地的射线结果代替（否则会被延迟与预测误差骗到）。
function GameCharacterBase:OnLocalHitConfirmed(HitTarget, DamageAmount, bHeadshot)
    -- 命中标记：爆头与普通命中形态不同（内容侧 HUD 负责渲染）
    self:ShowHitMarker(bHeadshot)

    -- 命中音效（"叮"声）：爆头用独立标签便于音频侧做区分
    local MarkerTagName = bHeadshot and "Audio.Combat.HitMarker.Head" or "Audio.Combat.HitMarker.Body"
    Audio.Play2D(self, Audio.RequestTag(MarkerTagName))
end

-- 击杀确认（**只有击杀者本端**收到）：击杀提示 + 击杀音效
function GameCharacterBase:OnLocalKillConfirmed(VictimActor)
    -- 击杀提示（屏幕中部"你击杀了 X"）：受害者名字从 PlayerState 取（不需要翻译）
    local VictimName = ""
    if VictimActor and VictimActor.PlayerState then
        VictimName = VictimActor.PlayerState:GetPlayerName()
    end
    self:ShowKillPrompt(VictimName)

    -- 击杀音效（与命中音效区分）
    Audio.Play2D(self, Audio.RequestTag("Audio.Combat.KillConfirmed"))

    -- TODO（表现）：连杀计数与连杀播报（后续里程碑）
end

-- =============================================================================
-- 受击表现（由 C++ CombatComponent 的受击委托派发进 Lua，当前为空壳预留）
-- =============================================================================

-- 响应受击事件
function GameCharacterBase:OnDamaged_Implementation(DamageAmount, HitBone, InstigatorActor)
    -- -------------------------------------------------------------------------
    -- 服务器权威：把"被攻击命中"上报给 GameMode（感染规则的触发入口）
    -- 本处只做"上报"，不做任何规则判断 —— 是否感染、感染成何种幽灵，一律由
    -- GameMode.lua 的对局规则脚本判定（规则可热更，改规则不必重编译 C++）。
    -- -------------------------------------------------------------------------
    if self:HasAuthority() then
        local AuthGameMode = UE.UGameplayStatics.GetGameMode(self)
        if AuthGameMode then
            -- 携带受害者控制器、攻击来源与本次伤害值；
            -- GameMode 侧会转发为 Lua 事件 OnIncomingAttackLanded（见 GameMode.lua）。
            AuthGameMode:ReportIncomingAttack(self:GetController(), InstigatorActor, DamageAmount)
        end
    end

    -- -------------------------------------------------------------------------
    -- 受击表现（各端）
    -- -------------------------------------------------------------------------
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
-- 死亡表现（由 C++ HealthComponent 的死亡委托经角色桥接派发进 Lua）
-- =============================================================================
-- 重要时序：本回调返回后，C++ 会立刻让死亡 Avatar 离场（卸下 + 销毁），
--           僵尸随后由规则脚本安排延迟复活。因此"必须在角色仍有效时完成"的表现
--           （如把网格体切开、生成尸体替身）要在此同步处理。
function GameCharacterBase:OnDeath_Implementation(KillerActor)
    -- 死亡即停火：避免"尸体还在连发"的表现与网络请求
    self:OnInputFireRelease()

    if self:IsLocallyControlled() then
        -- -------------------------------------------------------------------------
        -- 本地死亡演出：复仇镜头（把视角切到击杀者），让玩家看清是谁杀的
        -- -------------------------------------------------------------------------
        -- 说明：复活/重连拿到新角色时会把镜头切回自己（见 OnControllerReady），
        --       因此这里不需要（也无法）在这里安排"切回来"的时机。
        local PlayerController = self:GetController()
        if PlayerController and KillerActor and KillerActor ~= self then
            -- 0.35 秒混入：避免视角硬切造成的突兀感
            PlayerController:SetViewTargetWithBlend(KillerActor, 0.35)
        end

        -- TODO（表现）：画面黑白化/暗角、死亡音效
    else
        -- TODO（表现）：远端可见的尸体表现 —— 肉体松弛、碎裂特效、血液贴花
    end
end

-- 控制权就绪（C++ 在服务器的 PossessedBy 与客户端的 OnRep_Controller 里统一派发）
-- 用途一（客户端）：把"死亡时切给击杀者的复仇镜头"切回自己 —— 否则复活后会控制着新角色却看着别人。
-- 用途二（服务器）：向 GameMode 规则脚本上报"该玩家复活完成" —— 幽灵被枪杀后的
--       "重选窗口"必须开在新 Avatar 就绪之后（见 GameMode.lua 的 HandlePlayerRespawned）。
function GameCharacterBase:OnControllerReady()
    -- 服务器分支：上报复活完成（规则在 GameMode.lua；本处只做机械转发，不含任何判定）
    if self:HasAuthority() then
        local AuthGameMode = UE.UGameplayStatics.GetGameMode(self)
        if AuthGameMode and AuthGameMode.HandlePlayerRespawned then
            AuthGameMode:HandlePlayerRespawned(self:GetController())
        end
        return
    end

    if not self:IsLocallyControlled() then
        return
    end

    local PlayerController = self:GetController()
    if PlayerController then
        -- 混入时间 0 = 立刻切换：复活瞬间就应该看到自己的第一人称视角
        PlayerController:SetViewTargetWithBlend(self, 0.0)
    end
end

-- =============================================================================
-- 表现实现（音效已接入音频事件表；动画/后坐力/特效仍为占位，见各处 TODO）
-- =============================================================================

-- 第一人称开火表现（本地玩家）
-- 音效已接入：标签来自武器定义（内容侧填），"用哪个声音、多大声、是否空间化"由音频事件表决定
function GameCharacterBase:PlayFirstPersonFireFeedback(Weapon)
    if Weapon == nil then
        return
    end

    -- 开火音：挂在枪口挂点上播（跟随发声体，保证方位感随角色朝向变化）
    Audio.PlayAttached(self, Weapon.FireSoundEventTag, self.CharacterMesh, Weapon.MuzzleSocketName)

    -- TODO（表现）：按 Weapon.RecoilPitchKick / RecoilYawKick 做镜头后坐力、
    --               在 MuzzleSocketName 处生成枪口火焰与曳光弹、准星扩散
end

-- 第三人称开火表现（其他玩家的枪口火焰与音效）
function GameCharacterBase:PlayThirdPersonFireFeedback(Weapon)
    if Weapon == nil then
        return
    end

    -- 远端角色没有武器模型，音效直接在其身体位置播放（音频侧的空间衰减负责距离感）
    Audio.PlayAt(self, Weapon.FireSoundEventTag, self:GetActorLocation(), self)

    -- TODO（表现）：在远端角色的枪口挂点播枪口火焰（可由 GameplayCue 统一处理）
end

-- 空仓表现（本地）
function GameCharacterBase:PlayEmptyClipFeedback()
    local WeaponComponent = self:GetWeaponComponent()
    local Weapon = WeaponComponent and WeaponComponent:GetCurrentWeapon()
    if Weapon == nil then
        return
    end

    -- 空仓"枪机空击"声：挂在武器挂点上（不是枪口 —— 声音来自枪机，方位感更贴近手部）
    Audio.PlayAttached(self, Weapon.EmptySoundEventTag, self.CharacterMesh, Weapon.WeaponMeshAttachSocket)

    -- TODO（表现）：HUD 弹匣警示闪动
end

-- HUD 弹药刷新
function GameCharacterBase:UpdateAmmoHUD(Ammo, ReserveAmmo)
    -- TODO: 驱动 HUD 的弹匣/备弹文本与换弹进度（数据来源即本回调的两个参数）
end

-- 本地镜头受击抖动
function GameCharacterBase:PlayCameraShake(DamageAmount)
    -- TODO: 接入 UCameraShakeBase（受击强度按 DamageAmount 分级振幅）
end

-- HUD 命中标记（HitMarker）：由"服务器确认的命中"驱动，爆头形态不同
function GameCharacterBase:ShowHitMarker(bHeadshot)
    -- TODO: 驱动 HUD 的命中标记（普通 = 白叉、爆头 = 红叉 + 短促放大动效）
end

-- HUD 击杀提示（屏幕中部"你击杀了 X"）
function GameCharacterBase:ShowKillPrompt(VictimName)
    -- TODO: 驱动 HUD 的击杀提示条（文案走本地化表，如 UI.HUD.KillPrompt，参数为受害者名）
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
    -- TODO: 通知 UI 模块绑定 PlayerState 中的生命值与武器组件的弹药
end

return GameCharacterBase
