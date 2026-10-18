-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/UI/GameHUDWidget.lua
-- 模块路径: Lua.Core.UI.GameHUDWidget
--            （与 C++ UGameHUDWidget::LuaScriptPath 保持一致）
--
-- 竞技生化网游：HUD 控件表现脚本（血条 / 阵营 / 阶段倒计时 / 比分板）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（UGameHUDWidget）：只做"机制"——订阅数据源（玩家 ASC 生命属性、阵营、
--     对局阶段与比分）并把变化转发成下方同名事件；跨 Pawn 变异重生无需重绑。
--   * Lua 层（本文件）：全部"表现与提示"逻辑——血条增减动画、倒计时数字与格式、
--     比分板刷新、阵营变色提示、受击反馈。改完即热更生效，无需重编译 C++。
--
-- 本脚本可用到的 C++ 只读接口（供表现层取值）：
--   self:GetHealth() / self:GetMaxHealth() / self:GetHealthPercent()
--   self:GetMatchPhaseRemainingTime()
--   self:GetGamePlayerState()  -- 玩家状态（阵营 = :GetPlayerFaction()；
--                                --   进化等级 = :GetZombieLevel()，升级横幅可直接订阅
--                                --   PlayerState.OnZombieLevelChanged(NewLevel)，无需 C++ 转发）
--   self:GetGameGameState()    -- 对局状态（比分 = :GetTeamScore(阵营ID)）
--
-- 阵营 ID 约定（与 C++ GameFactionToTeamId 一致）：人类 = 0，生化幽灵 = 1。
-- =============================================================================

local GameHUDWidget = Class()

-- =============================================================================
-- 表现配置区（可热更调参）
-- =============================================================================
local CONFIG =
{
    -- 倒计时低时间告警阈值（秒）：剩余时间低于该值时进入"告警表现"（闪红 / 音效等）
    LowTimeWarningSeconds = 10.0,

    -- 生命低于该比例时进入"濒死表现"（屏幕边缘红晕 / 心跳音等）
    LowHealthPercent = 0.3,
}

-- =============================================================================
-- 控件构造（BlueprintImplementableEvent "Construct" 由 UnLua 接管）
--   说明：此处只做"一次性表现初始化"（缓存子控件引用、设定初值）。
--         所有随数据变化而更新的显示，一律由下方 OnHUD* 事件驱动，
--         且 C++ 在绑定成功时会主动推送一次当前值，故这里不必自己拉取数据。
-- =============================================================================
function GameHUDWidget:Construct()
    -- TODO（内容侧接入）：缓存 WBP 内子控件引用，例如
    --   self.HealthBar      = self.HealthBarWidget      -- 进度条
    --   self.HealthText     = self.HealthValueText      -- 数值文本
    --   self.PhaseText      = self.PhaseTimerText       -- 阶段/倒计时文本
    --   self.HumanScoreText = self.HumanScoreText       -- 人类比分
    --   self.ZombieScoreText= self.ZombieScoreText      -- 生化比分
    --   self.CrosshairImage = self.Crosshair            -- 准星（按阵营变色）

    -- 倒计时/告警的本地表现状态（仅在 Lua 侧维护，不参与任何权威逻辑）
    self.__bLowTimeWarned = false
    self.__bLowHealthWarned = false

    print("[UnLua] HUD 控件已构造")
end

-- =============================================================================
-- 数据变化事件（由 C++ UGameHUDWidget 转发；同名函数自动接管）
-- =============================================================================

-- 生命值 / 生命上限变化（首次绑定成功时也会推送一次当前值）
-- @param NewHealth 最新生命值
-- @param MaxHealth 最新生命上限
-- @param Delta     本次变化量（正为回血、负为受伤；0 表示"初始化同步"）
function GameHUDWidget:OnHUDHealthChanged(NewHealth, MaxHealth, Delta)
    local Percent = (MaxHealth > 0.0) and (NewHealth / MaxHealth) or 0.0

    -- TODO（表现）：血条插值动画 + 数值文本刷新
    -- if self.HealthBar then self.HealthBar:SetPercent(Percent) end
    -- if self.HealthText then self.HealthText:SetText(string.format("%d / %d", NewHealth, MaxHealth)) end

    -- 受伤飘字（Delta < 0 时；初始化同步 Delta == 0 不飘字）
    if Delta < 0.0 then
        -- TODO（表现）：在受击位置生成伤害数字，颜色按是否致命分级
    end

    -- 濒死表现（跨阈值时触发一次，避免每帧重复触发音效）
    local bLowHealth = Percent < CONFIG.LowHealthPercent and Percent > 0.0
    if bLowHealth ~= self.__bLowHealthWarned then
        self.__bLowHealthWarned = bLowHealth
        -- TODO（表现）：进入/退出濒死状态（屏幕红晕后处理、心跳音循环）
    end
end

-- 阵营变化（首次绑定时 OldFaction 与 NewFaction 相同，表示"当前阵营"）
-- @param NewFaction 新阵营（UE.EGameFaction.Human / UE.EGameFaction.Zombie）
-- @param OldFaction 旧阵营
function GameHUDWidget:OnHUDFactionChanged(NewFaction, OldFaction)
    -- 准星/边框按阵营变色：人类(蓝) / 生化(红)，供玩家一眼辨识敌我
    -- TODO（表现）：if self.CrosshairImage then self.CrosshairImage:SetColorAndOpacity(...) end

    -- 变异演出：真实"变身"时（New ~= Old）播放感染提示
    if NewFaction ~= OldFaction then
        -- TODO（表现）：全屏感染演出横幅、音效、镜头短暂变色
    end
end

-- 对局阶段变化（首次绑定时 OldPhase 与 NewPhase 相同，表示"当前阶段"）
-- @param NewPhase 新阶段（UE.EGameMatchPhase.Lobby / Warmup / Infection / Combat /
--                 Settlement / Intermission —— 六阶段回合循环，推进规则在 GameMode.lua）
-- @param OldPhase 旧阶段
function GameHUDWidget:OnHUDMatchPhaseChanged(NewPhase, OldPhase)
    -- 低时间告警状态按"是否进入新的倒计时阶段"复位（跨阶段不残留旧告警态）
    if NewPhase ~= OldPhase then
        self.__bLowTimeWarned = false
    end

    if NewPhase == UE.EGameMatchPhase.Warmup then
        -- TODO（表现）：显示"热身中"横幅 + 回合战绩/播报清零后的干净 HUD，启动热身倒计时
    elseif NewPhase == UE.EGameMatchPhase.Infection then
        -- TODO（表现）：播放"母体出现/感染爆发"横幅 + 音效（生化危机感的关键时刻）
    elseif NewPhase == UE.EGameMatchPhase.Combat then
        -- TODO（表现）：显示"人类抵抗"主倒计时（回合时长），切换战斗氛围 BGM 提示
    elseif NewPhase == UE.EGameMatchPhase.Settlement then
        -- TODO（表现）：弹出胜负结算横幅（人类胜/生化胜），隐藏战斗倒计时
    elseif NewPhase == UE.EGameMatchPhase.Intermission then
        -- TODO（表现）：显示"下一局等待"倒计时，提示回合重置完成
    elseif NewPhase == UE.EGameMatchPhase.Lobby then
        -- TODO（表现）：显示"等待玩家入场"提示，隐藏倒计时
    end
end

-- 阵营比分变化（无参数；收到后自行取值刷新）
function GameHUDWidget:OnHUDTeamScoresChanged()
    local GS = self:GetGameGameState()
    if not GS then
        return
    end

    -- 阵营 ID 与 C++ GameFactionToTeamId 约定一致：人类 = 0，生化幽灵 = 1
    local HumanScore = GS:GetTeamScore(0)
    local ZombieScore = GS:GetTeamScore(1)

    -- TODO（表现）：刷新比分板
    -- if self.HumanScoreText then self.HumanScoreText:SetText(tostring(HumanScore)) end
    -- if self.ZombieScoreText then self.ZombieScoreText:SetText(tostring(ZombieScore)) end
end

-- =============================================================================
-- 每帧表现刷新（仅"连续变化"的显示；不承载任何规则逻辑）
-- =============================================================================
function GameHUDWidget:OnHUDTick(DeltaSeconds)
    local GS = self:GetGameGameState()
    if not GS then
        return
    end

    -- 倒计时显示口径：除大厅（无限等待、无倒计时）外，六阶段都可能有倒计时；
    -- 剩余时间为 0（无结束时间戳）时不刷新文本，避免"00:00"残留。
    if GS:GetMatchPhase() == UE.EGameMatchPhase.Lobby then
        return
    end

    local Remaining = self:GetMatchPhaseRemainingTime()
    if Remaining <= 0.0 then
        return
    end

    -- 倒计时文本：把秒数格式化为 MM:SS
    local Minutes = math.floor(Remaining / 60)
    local Seconds = math.floor(Remaining % 60)
    -- TODO（表现）：if self.PhaseText then self.PhaseText:SetText(string.format("%02d:%02d", Minutes, Seconds)) end

    -- 低时间告警：跨阈值触发一次即可，避免逐帧重复播放音效
    local bLowTime = Remaining > 0.0 and Remaining <= CONFIG.LowTimeWarningSeconds
    if bLowTime ~= self.__bLowTimeWarned then
        self.__bLowTimeWarned = bLowTime
        -- TODO（表现）：进入/退出低时间告警（倒计时闪红、滴答音）
    end
end

return GameHUDWidget
