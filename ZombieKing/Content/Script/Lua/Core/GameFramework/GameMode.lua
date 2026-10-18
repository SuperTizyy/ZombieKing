-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/GameFramework/GameMode.lua
-- 模块路径: Lua.Core.GameFramework.GameMode
--            （由 C++ 的 AGameGameMode 通过 IUnLuaInterface 绑定；层级用 . 连接）
--
-- 竞技生化网游：对局规则脚本（服务器权威）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层：只做对局框架装配与生命周期事件派发（见 AGameGameMode / AGameGameState）——
--     装配类、给 Lua 派发事件、提供"框架级动作"（按阵营生成角色、感染重生）、权威计时；
--   * Lua 层（本文件）：全部"规则类"逻辑 —— 开局条件、阶段推进、母体选拔、感染规则、
--     胜负判定、比分结算、战绩计数、回合循环。写在这里的核心收益：线上可不停机热更、
--     无需重新编译 C++。
--
-- 对局阶段（六阶段回合循环；阶段枚举是 C++ 复制契约，推进规则全在本脚本）：
--   Lobby(大厅等人) → Warmup(热身) → Infection(母体选拔+变身选择窗口)
--   → Combat(人类抵抗主战斗) → Settlement(结算展示) → Intermission(休整) → 回到 Warmup
--   玩法门控口径：命中即感染 / 伤害与战绩统计，仅在 Infection 与 Combat 两阶段开放
--   （见 IsCombatEnabledPhase；Warmup 全员人类无感染；结算/休整不做有效判定）。
--
-- 本脚本依赖的 C++ 契约（均已提供，改动数据结构才需动 C++）：
--   AGameGameState（经 UE.UGameplayStatics.GetGameState(self) 取得）
--     :GetMatchPhase()                       -- 只读查询当前阶段
--     :SetMatchPhase(NewPhase)               -- 服务器权威写入阶段
--     :SetMatchPhaseEndServerTime(时间戳)     -- 设置该阶段倒计时结束时间（0 = 无倒计时）
--     :GetTeamScore(TeamId)                  -- 只读查询阵营比分
--     :AddTeamScore(TeamId, Delta)           -- 服务器权威累加比分
--     :ClearKillFeed()                       -- 清空击杀播报（回合开局）
--     .PlayerArray                           -- 在场玩家 PlayerState 列表（1 基索引，:Num() 取数量）
--     .OnGameMatchPhaseExpired               -- 阶段倒计时到期事件（由 C++ 权威计时器广播）
--   AGamePlayerState（经 Controller.PlayerState 取得）
--     :AddKill() / :AddDeath()               -- 战绩 +1（服务器权威；击杀/死亡计数入口在本脚本）
--     :AddDamageDealt(整数伤害)               -- 累计造成伤害（只增不减）
--     :ResetRoundStats()                     -- 清空本回合战绩（回合开局统一调用；含等级与感染计数归零）
--     :GetZombieLevel()                      -- 当前进化等级（0=人类；1..6=幽灵）
--     :GetRoundInfectionCount()              -- 本回合感染计数（升级进度）
--     :AddInfectionCount(1)                  -- 感染计数 +1（感染成功时调用）
--     :SetRoundInfectionCount(0)             -- 升级后清零感染进度
--   AGameCharacterBase（攻击者/变身后新 Avatar，经 Pawn 取得）
--     :SetZombieLevel(等级)                   -- 进化落地入口（服务器权威；血量增量+体型+模型，数值表在角色定义资产）
--     :WasLastHitMelee()                     -- 最近一次受击是否近战（服务器读；"刀杀禁令"判定源）
--   AGameGameMode（经 UE.UGameplayStatics.GetGameMode(self) 取得；self 即本脚本绑定的 GameMode）
--     :InfectPlayer(Controller, ZombieClass) -- 把玩家感染重生为生化幽灵（传 nil 用阵营默认类；
--     :                                        现主要用于 C++ 变身选择完成后的生成链路）
--     :BeginGhostSelection(Controller, 秒)   -- 发起"变身选择"（CF 式）：阵营立即切生化 + 弹框
--     :                                        从玩家仓库挑幽灵角色；超时自动用保底角色
--     :ReopenGhostSelection(Controller, 秒)  -- 发起"幽灵重选"窗口（复活后几秒换角色机会）：
--     :                                        不卸 Avatar、超时=保留当前角色（与首次变身三点差异）
--     :OnGhostTransformed(...)               -- 变身/重选完成事件，由本脚本实现（见下方同名函数）
--     :RespawnPlayerAsFaction(Controller, Faction, PawnClass) -- 按阵营重生
--     :ReportIncomingAttack(...)             -- 由受击角色脚本调用的"命中上报"入口
--     :OnIncomingAttackLanded(...)           -- 命中落地事件，由本脚本实现（见下方同名函数）
--     :ScheduleRespawn(Controller, 延迟秒)    -- 安排延迟复活（按"当前阵营"在出生点重生）
--     :SetPlayerAsSpectator(Controller)      -- 把玩家转为观察者（人类死亡时使用）
--     :OnPlayerDied(...)                     -- 死亡落地事件，由本脚本实现（见下方同名函数）
--     :DispatchGmCommand(CommandId, Args)    -- GM 命令派发（服务器权威；由 C++ GM 服务解析控制台 "GK.xxx" 后调用）
--     :OnGmCommand(...)                      -- GM 命令落地事件，由本脚本实现（见文末 GM_COMMANDS 表）
--
-- 玩法规则（已由用户拍板，作为本脚本实现基准）：
--   1) 感染触发：生化幽灵"攻击命中"人类即感染（非接触、非技能）；
--   2) 胜负判定：人类全部被感染 → 生化阵营胜；Combat 倒计时结束仍有幸存人类 → 人类阵营胜；
--   3) 母体产生：热身结束后进入感染阶段，按在场人数比例随机选出第一批母体；
--   4) 变身选择（CF 式）：被选拔为母体 / 被感染的玩家弹出选择框，从自己**仓库中拥有的幽灵角色**
--      里选一个变身；选择框带倒计时（CONFIG.GhostSelectionDuration），超时自动变"基础幽灵"
--      （人人免费拥有的保底角色）；选择期间阵营已即时切为生化；
--   5) 死亡处置：生化幽灵被**枪械/投射物**击杀 → 延迟复活且仍为生化幽灵，但**进化等级重置为
--      1 级**（血量/体型/模型全部还原 1 级形态，感染进度清零 —— 进化增益不跨死亡）；
--      生化幽灵被**近战（刀/爪）**击杀 → CF 式"刀杀禁令"：本回合禁复活、转入观战；
--      人类死亡（如摔死）→ 转为观察者，本回合不再复活（回合结束由重置流程统一拉回战场）；
--   6) 战绩口径（K/D/伤害，与击杀播报同一判定口径）：
--      - 死亡：战斗开放阶段内玩家死亡一律记 1 次死亡（含"被感染倒下"，与 CF 口径一致）；
--      - 击杀：仅"击杀播报成立"时给击杀者记 1 次击杀（感染不算击杀；自杀/环境死亡不记）；
--      - 伤害：战斗开放阶段内，玩家来源的命中按实际生效伤害取整累计到攻击者战绩；
--      - 每回合开局（进入 Warmup）统一清空全部玩家战绩与击杀播报。
--   7) 进化等级（CF 式 6 级轨，所有生化幽灵共用；数值表在各自角色定义资产 ZombieLevels）：
--      - 开局被选拔的母体从 MotherStartLevel(3) 起步；被感染者从 InfectedStartLevel(1) 起步；
--      - 升级规则：每感染 InfectionsPerLevel 人升 1 级（感染计数随升级清零重计），封顶 ZombieLevelMax(6)；
--      - 升级落地（血量增量/体型放大/模型小变化）由角色 C++ SetZombieLevel 执行，等级随 PlayerState 复制；
--        **被枪杀复活重置为 1 级**（见规则 5），被刀杀直接禁复活；回合结束随战绩清零一并归零；
--      - 血量/体型/模型的具体数值全部在内容资产（逐幽灵不同），本脚本只定"升几级、何时升"。
--   8) 幽灵重选（用户定案；本游戏"母体=幽灵"统称，故适用于**所有**生化幽灵）：
--      每次被枪械击杀复活后，获得 CONFIG.GhostReSelectionDuration 秒的"重选角色"窗口 ——
--      复用变身选择弹框（OnGhostSelectionRequested），从仓库整表重选；
--      **重选只换角色，进化等级不受影响**（严格 1 级口径：等级只在死亡时重置、只在感染时升级）；
--      超时不选 = 保留当前角色继续战斗（重选是机会不是义务）；窗口期间照常行动（不卸 Avatar）；
--      窗口期间再次死亡 → 旧窗口先行取消，枪杀则复活后重新开窗（"每次被击杀都有机会"）。
--   9) GM 调试命令（本里程碑）：服务器控制台 "GK <命令> [参数...]"（仅非 Shipping 构建存在该入口；
--      C++ GM 服务只做"解析 + 找到权威 GameMode + 转发"）→ 本脚本 OnGmCommand 接管，
--      全部命令行为在文末 GM_COMMANDS 表 —— 新增/修改命令只热更本文件、不重编译 C++。
-- =============================================================================

local GameMode = Class()

-- =============================================================================
-- 规则配置区（可热更调参）
--   说明：本项目的运营节奏需要不停机调参，因此所有"会随运营调整"的数值集中在此处。
--         调整这些数值只改本文件即可生效，无需重新编译 C++、无需重打内容包。
-- =============================================================================
local CONFIG =
{
    -- ------------------------- 开局条件 -------------------------
    -- 进入对局所需的最少在场人数：达到即自动开局（大厅无限等待，见 OnPlayerEnteredMatch）
    PlayersToStartMatch = 2,

    -- ------------------------- 阶段时长（秒） -------------------------
    -- 热身时长：全员人类、开放出生与走位，不产生母体；到点进入感染阶段
    WarmupDuration = 5.0,
    -- 感染阶段时长：母体选拔 + 变身选择窗口。
    -- 约束：必须 ≥ GhostSelectionDuration + 少量缓冲，保证"母体全部完成变身"发生在本阶段内，
    --       否则未完成的玩家会带着"选择中"状态进入 Combat（机制上仍会超时保底，不会卡死）。
    InfectionDuration = 8.0,
    -- 人类抵抗（主战斗）时长：到点仍有幸存人类则人类胜（否则生化已把人类感染完）
    RoundDuration = 180.0,
    -- 结算展示时长：胜负已定、比分已写入，玩家暂留战场供观战/看结算面板；到点重置玩家
    SettlementDuration = 6.0,
    -- 局间休整时长：全员已重置回人类；到点人数足够则自动开下一局，否则回大厅继续等待
    IntermissionDuration = 8.0,

    -- ------------------------- 母体选拔 -------------------------
    -- 每多少名玩家产生 1 名母体（向上取整）。例：8 → 16 人时产生 2 名母体
    MotherPerPlayers = 8,
    -- 母体最少数量：即便人数很少也至少选出 1 名母体
    MinMothers = 1,

    -- ------------------------- 死亡与复活 -------------------------
    -- 生化幽灵被击杀后的复活延迟（秒）：延迟结束在出生点复活，且仍为生化幽灵。
    -- 调大 → 人类杀僵尸的收益更高；调小 → 僵尸压迫感更强。
    ZombieRespawnDelay = 5.0,

    -- 幽灵重选窗口（秒）：生化幽灵**每次被枪械击杀复活后**，弹出几秒的"重选角色"窗口
    -- （复用变身选择弹框；重选只换角色、进化等级不受影响；超时不选=保留当前角色继续战斗）。
    -- 设 0 等效关闭该功能（窗口过短无意义；Lua 规则会跳过发起，见 HandlePlayerRespawned）。
    GhostReSelectionDuration = 5.0,

    -- ------------------------- 变身选择（CF 式） -------------------------
    -- 变身选择框的倒计时（秒）：开局被选拔为母体 / 被攻击感染的玩家，在此时间内
    -- 从自己仓库中拥有的幽灵角色里挑一个变身；超时自动用保底角色（基础幽灵，人人免费拥有）。
    -- 调大 → 玩家挑选更从容，但变身等待期更长；调小 → 节奏紧凑但容易超时。
    GhostSelectionDuration = 5.0,

    -- ------------------------- 进化等级（CF 式 6 级轨） -------------------------
    -- 等级轨封顶（所有生化幽灵共用；须与角色定义资产 ZombieLevels 表长度对齐，表短了按缺级降级）。
    ZombieLevelMax = 6,
    -- 开局被选拔的母体起步等级（CF：母体天生更强）。
    MotherStartLevel = 3,
    -- 被感染变身的普通幽灵起步等级。
    InfectedStartLevel = 1,
    -- 每感染多少名人类升 1 级（感染计数随升级清零重计）。
    -- 调小 → 幽灵滚雪球更快（压迫感强）；调大 → 人类压制期更长。
    InfectionsPerLevel = 2,

    -- ------------------------- 阵营 ID（与 C++ GameFactionToTeamId 保持一致） -------------------------
    -- C++ 约定：人类 = 0、生化幽灵 = 1（见 Source/ZombieKing/Character/GameFaction.h）。
    -- 此处冗余一份常量是为了让 Lua 规则可直接调用 GameState:AddTeamScore 而无需跨语言查询。
    HumanTeamId = 0,
    ZombieTeamId = 1,
}

-- =============================================================================
-- 阶段门控（口径集中一处，避免各处散落比较）
-- =============================================================================

-- 战斗是否开放：仅"感染爆发"与"人类抵抗"两阶段开放命中感染、伤害与战绩统计。
-- Warmup（全员人类）与 Settlement/Intermission（胜负已定）期间一律拒绝。
local function IsCombatEnabledPhase(Phase)
    return Phase == UE.EGameMatchPhase.Infection
        or Phase == UE.EGameMatchPhase.Combat
end

-- =============================================================================
-- 生命周期
-- =============================================================================
function GameMode:ReceiveBeginPlay()
    -- 母体选拔登记簿（服务器侧 Lua 状态）：SelectAndInfectMothers 写入、OnGhostTransformed 消费。
    -- 用途：区分"开局选拔的母体（3 级起步）"与"战斗中被感染的普通幽灵（1 级起步）"——
    --       两者都走同一条变身选择链路，完成事件本身不带"选拔来源"，只能在此打标。
    -- 清理：回合重置（ResetAllPlayersToHuman）整表作废，防止"选拔未完成即被回合重置"的陈旧标记
    --       在下一回合误发母体起步等级。
    self.MotherLedger = {}

    -- 重选待办表（服务器侧 Lua 状态）：幽灵被枪杀 → PendingReSelection[Controller]=true；
    -- 复活完成（HandlePlayerRespawned）消费并开启重选窗口，同时登记 ReSelectionInFlight
    -- 供 OnGhostTransformed 区分"重选完成（不换等级）"与"首次变身（按起步等级）"。
    -- 回合重置整表作废（防止"死亡待复活"跨回合误开窗口）。
    self.PendingReSelection = {}
    self.ReSelectionInFlight = {}

    -- 订阅"阶段倒计时到期"事件：这是 C++ 权威计时器给出的"到点通知"，
    -- 阶段推进规则（热身→感染→抵抗→结算→休整）由本脚本实现（见 OnMatchPhaseExpired）。
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if GameState then
        -- 先移除再绑定，避免重进关卡/热重载导致的重复订阅（同一次到点被推进两次）
        GameState.OnGameMatchPhaseExpired:Remove(self, GameMode.OnMatchPhaseExpired)
        GameState.OnGameMatchPhaseExpired:Add(self, GameMode.OnMatchPhaseExpired)
    end

    -- 对局启动：先进入"大厅等待"阶段，等待玩家入场（无倒计时上限）
    self:EnterPhase(UE.EGameMatchPhase.Lobby, 0.0)
end

-- =============================================================================
-- 框架事件（由 C++ AGameGameMode 的 BlueprintImplementableEvent 派发，同名函数自动接管）
-- =============================================================================

-- 玩家入场：开局条件规则的落点
function GameMode:OnPlayerEnteredMatch(NewPlayer)
    -- 人数达到阈值即开局；人数不足则维持大厅等待（本函数在每次有人入场时被调用）
    self:TryStartMatch()
end

-- 玩家离场：人数不足回退 / 对局中"人类全灭"判定的落点
function GameMode:OnPlayerLeftMatch(Exiting)
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    -- 战斗开放阶段：离场可能导致"人类全灭"，此时应判定为生化阵营胜。
    -- 注意：Logout 派发时离场者的 PlayerState 可能尚未从 PlayerArray 移除，此判定存在轻微时序误差；
    --       主路径（人类被感染）由 OnIncomingAttackLanded 中的判定覆盖，本处仅作兜底。
    if IsCombatEnabledPhase(GameState:GetMatchPhase()) then
        self:CheckAndResolveWinCondition()
    end
end

-- 攻击命中落地：感染规则 + 伤害战绩的落点（由 C++ ReportIncomingAttack 转发而来；服务器权威）
function GameMode:OnIncomingAttackLanded(VictimController, InstigatorActor, DamageAmount)
    -- 双重校验：仅服务器受理、且仅战斗开放阶段才受理（热身/结算/休整期间拒绝）
    if not self:HasAuthority() then
        return
    end
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState or not IsCombatEnabledPhase(GameState:GetMatchPhase()) then
        return
    end
    if not VictimController then
        return
    end

    local VictimPS = VictimController.PlayerState
    if not VictimPS then
        return
    end

    -- 攻击来源归属：InstigatorActor 为发起攻击的 Avatar（Pawn）；非 Pawn（如环境伤害）视为无主
    local AttackerPS = nil
    if InstigatorActor and InstigatorActor:IsA(UE.APawn) then
        AttackerPS = InstigatorActor.PlayerState
    end

    -- -------------------------------------------------------------------------
    -- 伤害战绩：放在阵营判定**之前**统计 —— 无论受害者是人类还是生化幽灵，
    -- 只要命中由玩家发起且实际生效，就应计入攻击者的"造成伤害"。
    -- 口径：DamageAmount 来自属性变化回调 = 实际生效伤害（已按目标剩余生命钳制），
    --       取整后交给 PlayerState（C++ 侧拒绝非正数，0 伤害自然不计数）。
    -- -------------------------------------------------------------------------
    if AttackerPS and AttackerPS ~= VictimPS then
        AttackerPS:AddDamageDealt(math.floor(DamageAmount))
    end

    -- 规则 1：受害者必须是人类（已是生化幽灵者不再重复感染）
    if VictimPS:GetPlayerFaction() ~= UE.EGameFaction.Human then
        return
    end

    -- 规则 2：攻击者必须是生化幽灵（人类之间的友伤不触发感染）。
    local AttackerFaction = AttackerPS and AttackerPS:GetPlayerFaction() or UE.EGameFaction.None
    if AttackerFaction ~= UE.EGameFaction.Zombie then
        return
    end

    -- 规则 3：命中即感染 —— 发起"变身选择"（CF 式：受害者弹出选择框，从自己仓库中
    -- 拥有的幽灵角色里挑一个变身；超时自动用保底角色）。
    -- 阵营在此调用内**立即**切为生化（胜负判定/重复感染拦截即刻生效，无需等选完）。
    local bInfected = self:BeginGhostSelection(VictimController, CONFIG.GhostSelectionDuration)
    if bInfected then
        -- ---------------------------------------------------------------------
        -- 进化升级判定（感染计数规则）：感染成功 → 计数 +1；攒满 InfectionsPerLevel
        -- → 升 1 级并清零重计；封顶 ZombieLevelMax 后计数继续累计（供 HUD 进度条展示）但不再升级。
        -- 升级落地走攻击者当前 Avatar 的 C++ 入口（血量增量/体型/模型按角色定义资产执行）。
        -- ---------------------------------------------------------------------
        AttackerPS:AddInfectionCount(1)
        local CurrentLevel = AttackerPS:GetZombieLevel()
        if AttackerPS:GetRoundInfectionCount() >= CONFIG.InfectionsPerLevel
            and CurrentLevel < CONFIG.ZombieLevelMax then
            AttackerPS:SetRoundInfectionCount(0)
            InstigatorActor:SetZombieLevel(CurrentLevel + 1)
            print(string.format("[Evolution] %s 感染数达标，进化到 %d 级",
                AttackerPS:GetPlayerName(), CurrentLevel + 1))
        end

        -- 感染成功后立即判定胜负：若受害者是最后一名人类，则生化阵营即刻获胜
        self:CheckAndResolveWinCondition()
    end
end

-- 变身完成（由 C++ 在玩家选定/超时保底并生成所选幽灵后派发；服务器权威）。
-- 本脚本职责：按"起步等级"落地进化状态（母体 3 级 / 普通感染 1 级），并做连锁表现。
function GameMode:OnGhostTransformed(Controller, GhostClass)
    if not Controller then
        return
    end

    -- 分支 1：幽灵重选完成 —— **只换角色、不动进化等级**（用户定案"严格 1 级"：
    -- 等级在死亡时已重置为 1，重选期间不变；新 Avatar 由 C++ GAS 上下文按 PlayerState
    -- 当前等级自动摆好血量/体型/模型，这里无需任何等级操作）。
    if self.ReSelectionInFlight and self.ReSelectionInFlight[Controller] then
        self.ReSelectionInFlight[Controller] = nil
        print(string.format("[ReSelect] %s 已重选幽灵角色：%s（等级不变）",
            Controller.PlayerState and Controller.PlayerState:GetPlayerName() or "?",
            GhostClass and GhostClass:GetName() or "?"))
        return
    end

    -- 分支 2：首次变身完成 —— 起步等级判定：消费母体登记簿标记（一次性，防复用）；
    -- 无标记 = 战斗中被感染的普通幽灵
    local bWasMother = self.MotherLedger and self.MotherLedger[Controller] == true
    if bWasMother then
        self.MotherLedger[Controller] = nil
    end
    local StartLevel = bWasMother and CONFIG.MotherStartLevel or CONFIG.InfectedStartLevel

    -- 进化落地（血量增量/体型/模型由 C++ 按角色定义资产执行；等级随 PlayerState 复制全网）。
    -- 1 级起步也要写入状态：让 PlayerState 等级脱离"0=人类"哨兵值（HUD/比分板按等级显示幽灵）。
    local NewPawn = Controller:GetPawn()
    if NewPawn then
        NewPawn:SetZombieLevel(StartLevel)
    end

    print(string.format("[Match] %s 已变身为 %s（%s，%d 级起步）",
        Controller.PlayerState and Controller.PlayerState:GetPlayerName() or "?",
        GhostClass and GhostClass:GetName() or "?",
        bWasMother and "母体" or "感染", StartLevel))
end

-- 死亡落地：死亡处置规则 + 击杀/死亡战绩的落点（由 C++ ReportPlayerDeath 转发而来；服务器权威）
-- 前置说明：C++ 在此处只派发事件、不移除角色；"把尸体移出战场"由下面两个规则动作负责：
--   ScheduleRespawn（僵尸复活前先移除）与 SetPlayerAsSpectator（人类转观察者时移除）。
function GameMode:OnPlayerDied(VictimController, KillerActor)
    -- 仅服务器执行处置
    if not self:HasAuthority() then
        return
    end
    if not VictimController then
        return
    end

    local GameState = UE.UGameplayStatics.GetGameState(self)

    -- 非战斗开放阶段（大厅/热身/结算/休整）发生的死亡不做战斗处置：
    -- 按"转观察者"兜底移除角色，避免出现"零血尸体仍被控制、还能移动开火"的异常状态。
    if not GameState or not IsCombatEnabledPhase(GameState:GetMatchPhase()) then
        self:SetPlayerAsSpectator(VictimController)
        return
    end

    local VictimPS = VictimController.PlayerState
    if not VictimPS then
        return
    end

    -- -------------------------------------------------------------------------
    -- 死亡战绩：战斗阶段内的死亡**一律**记 1 次，含"被感染倒下"
    -- （人类被生化打死同样算阵亡，与 CF 生化口径一致）。
    -- 必须放在下方"感染特例 return"之前，否则被感染的人类会漏记死亡。
    -- -------------------------------------------------------------------------
    VictimPS:AddDeath()

    -- 取击杀者归属（KillerActor 为发起攻击的 Avatar；非 Pawn 则视为未知来源）
    local KillerFaction = UE.EGameFaction.None
    local KillerController = nil
    local KillerPS = nil
    if KillerActor and KillerActor:IsA(UE.APawn) then
        KillerPS = KillerActor.PlayerState
        if KillerPS then
            KillerFaction = KillerPS:GetPlayerFaction()
        end
        KillerController = KillerActor:GetController()
    end

    -- 特例：人类被生化幽灵"当场打死"时，感染规则（攻击命中即感染）会同时把该玩家变异为生化幽灵，
    -- 因此这里不再按"人类死亡"处理 —— 否则会先转观察者、再被变异复活，两种状态互相打架。
    -- 注意：死亡数已在上面记过；"感染不算击杀"，所以击杀者与播报都不动。
    if VictimPS:GetPlayerFaction() == UE.EGameFaction.Human
        and KillerFaction == UE.EGameFaction.Zombie then
        return
    end

    -- -------------------------------------------------------------------------
    -- 击杀播报 + 击杀战绩：两者共用同一判定口径（"什么时候算一次击杀"是规则，就在此处）。
    -- 分工：名字/阵营组装与复制裁剪由 C++ GameState:ReportKill 负责，Lua 只传控制器与爆头标记；
    --       击杀数由击杀者 PlayerState:AddKill 记录（自杀不记，防"刷击杀"）。
    -- 是否爆头：取受害者"最近一次受击是否为头部"（C++ 在受击时已缓存该结论，
    --           因为死亡事件本身不携带命中细节）。
    -- -------------------------------------------------------------------------
    local bSelfKill = KillerController == VictimController
    if KillerPS and KillerController and not bSelfKill then
        KillerPS:AddKill()
    end

    local VictimPawn = VictimController.Pawn
    local bHeadshot = false
    if VictimPawn then
        bHeadshot = VictimPawn:WasLastHitHeadshot()
    end

    GameState:ReportKill(KillerController, VictimController, bHeadshot)

    if VictimPS:GetPlayerFaction() == UE.EGameFaction.Zombie then
        -- -------------------------------------------------------------------------
        -- 规则 4a：生化幽灵被击杀的两种归宿（CF 式"刀杀禁令" + 用户定案的"枪杀降级还原"）：
        --   被近战（刀/爪）致命一击击杀 → 本回合禁复活，直接转观察者（人类刀的硬核收益）；
        --   被枪械/投射物/环境击杀       → 延迟复活、仍为生化幽灵，但进化等级重置为 1 级
        --     （血量/体型/模型随之还原 1 级形态，感染进度清零 —— 进化增益不跨死亡保留），
        --     并且**复活后获得几秒的"重选角色"窗口**（见 HandlePlayerRespawned）。
        -- "是否近战"读受害者角色缓存（受击时由伤害规格的动态资产标签判定，见 WasLastHitMelee）。
        -- -------------------------------------------------------------------------
        -- 先行取消可能还开着的重选窗口（上一次复活后没选完又被杀）：
        -- 不取消的话，旧窗口的超时/迟到提交会作用在"已再次死亡"的玩家身上（状态错乱）。
        VictimController:CancelGhostSelectionServer()

        local bMeleeKill = VictimPawn and VictimPawn:WasLastHitMelee()
        if bMeleeKill then
            print(string.format("[Match] %s 被刀杀，本回合禁复活（转观战）",
                VictimPS:GetPlayerName()))
            self:SetPlayerAsSpectator(VictimController)
        else
            self:ScheduleRespawn(VictimController, CONFIG.ZombieRespawnDelay)
            -- -----------------------------------------------------------------
            -- 被枪杀复活的"归零还原"（用户定案规则）：等级重置为 1 级、感染进度清零，
            -- 血量/体型/模型随等级状态自动回到 1 级形态（进化增量不跨死亡保留）。
            -- 顺序说明：放在 ScheduleRespawn **之后** —— 此时旧 Avatar 已被移出战场，
            --   等级事件的"体型/模型还原"不会作用在尸体上；复活生成的新 Avatar 在 GAS
            --   上下文初始化时按 PlayerState 当前等级（=1）摆好形态，天然就是 1 级状态。
            -- -----------------------------------------------------------------
            VictimPS:SetZombieLevel(1)
            VictimPS:SetRoundInfectionCount(0)

            -- 登记"复活后重选"待办：复活完成时（角色 OnControllerReady 服务器分支 →
            -- HandlePlayerRespawned）开启几秒的重选窗口。刀杀不登记（本回合已禁复活）。
            self.PendingReSelection[VictimController] = true
        end
    else
        -- 规则 4b：人类死亡（如摔死 / 其他非生化来源）→ 转为观察者，本回合不再复活；
        --          回合结束时由 ResetAllPlayersToHuman 统一把观察者拉回战场。
        self:SetPlayerAsSpectator(VictimController)
    end
end

-- 复活完成（由角色脚本 GameCharacterBase.lua 在服务器 PossessedBy 派发 OnControllerReady 时上报）
-- 规则落点：幽灵被枪杀登记的"重选窗口"必须开在**新 Avatar 就绪之后**（带着角色选、选完即换），
--          这里消费待办并调 C++ ReopenGhostSelection 弹几秒的重选框。
-- 注意：本函数在每次附身完成时都会被调用（含开局首次生成），靠"待办表有无登记"区分，
--       未登记者直接忽略 —— 开局变身走的是 MotherLedger/感染链路，不经过这里。
function GameMode:HandlePlayerRespawned(Controller)
    if not self:HasAuthority() or not Controller then
        return
    end
    if not self.PendingReSelection or self.PendingReSelection[Controller] ~= true then
        return
    end

    -- 消费待办（一次性）：无论是否成功开窗都不重登记 —— 窗口开启失败（如恰逢回合重置
    -- 把阵营切回人类）时，本次复活就静默放弃重选机会，不会残留状态。
    self.PendingReSelection[Controller] = nil

    if CONFIG.GhostReSelectionDuration <= 0 then
        return -- 配置为 0 = 关闭重选功能（运营可热更开关）
    end

    if self:ReopenGhostSelection(Controller, CONFIG.GhostReSelectionDuration) then
        -- 登记"重选进行中"：完成事件 OnGhostTransformed 据此走"只换角色不动等级"分支
        self.ReSelectionInFlight[Controller] = true
        print(string.format("[ReSelect] %s 复活完成，开启 %.1f 秒幽灵重选窗口",
            Controller.PlayerState and Controller.PlayerState:GetPlayerName() or "?",
            CONFIG.GhostReSelectionDuration))
    end
end

-- =============================================================================
-- 阶段推进（规则实现处；数值与判定条件均在本脚本内，属可热更范围）
-- =============================================================================

-- 进入指定阶段，并可指定该阶段的持续秒数（0 表示无倒计时，常用于大厅等待）
function GameMode:EnterPhase(NewPhase, DurationSeconds)
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        print("[Match] 未取到 GameState，阶段切换被跳过: " .. tostring(NewPhase))
        return
    end

    -- 服务器权威写入当前阶段（该接口内部有 HasAuthority 校验，客户端调用会被安全忽略）
    GameState:SetMatchPhase(NewPhase)

    -- 回合真正开始 = 进入热身阶段：清空击杀播报与全部玩家战绩，
    -- 避免上一回合的播报/战绩残留到新一回合的 HUD 与计分板上。
    if NewPhase == UE.EGameMatchPhase.Warmup then
        GameState:ClearKillFeed()
        self:ResetAllRoundStats()
    end

    if DurationSeconds and DurationSeconds > 0 then
        -- 以"服务器世界时间"为基准写入结束时间戳：
        -- 客户端据此本地倒计时，避免服务器逐帧复制剩余时间（省带宽、防抖动）；
        -- 同时 C++ 会据此武装权威定时器，到点广播 OnGameMatchPhaseExpired 交给本脚本推进。
        -- 注意：C++ 只维护**一个**定时器句柄，每次重设结束时间都会先清除旧定时器 ——
        --       因此"提前结算"（如感染阶段内人类已全灭）不会残留旧阶段的到期回调。
        local ServerTime = UE.UGameplayStatics.GetTimeSeconds(self)
        GameState:SetMatchPhaseEndServerTime(ServerTime + DurationSeconds)
    else
        -- 0 = 该阶段无倒计时（典型如大厅无限等待）；C++ 会清除权威定时器
        GameState:SetMatchPhaseEndServerTime(0.0)
    end
end

-- 清空全部在场玩家的回合战绩（进入 Warmup 时调用；与击杀播报清空同一时机）
function GameMode:ResetAllRoundStats()
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    local Arr = GameState.PlayerArray
    for i = 1, Arr:Num() do
        local PS = Arr[i]
        if PS then
            PS:ResetRoundStats()
        end
    end
end

-- 开局条件校验：人数足够且当前处于"可开局"阶段（大厅/休整），则开始对局；否则维持等待
function GameMode:TryStartMatch()
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    -- 只有"大厅等待"与"局间休整"两个阶段允许开局；
    -- 回合已在进行中（热身/感染/抵抗/结算）则忽略，防止重复开局
    local Phase = GameState:GetMatchPhase()
    if Phase ~= UE.EGameMatchPhase.Lobby and Phase ~= UE.EGameMatchPhase.Intermission then
        return
    end

    if self:CountPlayers() < CONFIG.PlayersToStartMatch then
        -- 人数不足：若正处于休整倒计时，则转为"大厅无限等待"继续凑人
        --（已在 Lobby 时本调用幂等短路，不会重复广播阶段变化）
        self:EnterPhase(UE.EGameMatchPhase.Lobby, 0.0)
        return
    end

    self:StartMatch()
end

-- 开局：进入热身阶段（母体选拔推迟到热身结束的"感染阶段"，见 OnMatchPhaseExpired）
function GameMode:StartMatch()
    -- 推进到热身阶段（带倒计时）：全员人类出生、走位准备；
    -- 进入热身时同步清空击杀播报与回合战绩（见 EnterPhase 的 Warmup 分支）
    self:EnterPhase(UE.EGameMatchPhase.Warmup, CONFIG.WarmupDuration)
end

-- 进入感染阶段：按比例选拔母体并发起变身选择
function GameMode:BeginInfectionPhase()
    -- 1) 推进到感染阶段（带倒计时）：命中即感染自本阶段开放；
    --    倒计时到点后无论变身选择是否全部完成，都推进到主战斗阶段
    --    （未完成者由 C++ 的独立超时保底兜底，不会卡在"选择中"状态）
    self:EnterPhase(UE.EGameMatchPhase.Infection, CONFIG.InfectionDuration)

    -- 2) 按在场人数比例随机选出母体，并对她们发起变身选择
    self:SelectAndInfectMothers()

    -- 3) 兜底判定：极端人数下（如仅 1 人且被选为母体）可能瞬间满足生化胜条件
    self:CheckAndResolveWinCondition()
end

-- 按人数比例随机选拔母体并发起"变身选择"（CF 式：被选中者弹框从仓库挑母体角色）
function GameMode:SelectAndInfectMothers()
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    -- 收集在场玩家的控制器（PlayerArray 为 UnLua 的 1 基索引 TArray，用 :Num() 取数量）
    local Controllers = {}
    local Arr = GameState.PlayerArray
    for i = 1, Arr:Num() do
        local PS = Arr[i]
        local PC = PS and PS:GetPlayerController()
        if PC then
            Controllers[#Controllers + 1] = PC
        end
    end

    local Total = #Controllers
    if Total <= 0 then
        return
    end

    -- 母体数量 = 向上取整(总人数 / 每多少人一个)，且不少于 MinMothers、不超过总人数
    local MotherCount = math.ceil(Total / CONFIG.MotherPerPlayers)
    if MotherCount < CONFIG.MinMothers then
        MotherCount = CONFIG.MinMothers
    end
    if MotherCount > Total then
        MotherCount = Total
    end

    -- Fisher-Yates 洗牌后用前 N 名作母体：随机源使用引擎 UFUNCTION（RandomIntegerInRange），
    -- 避免依赖 Lua math.random 的平台差异与未播种问题。
    for i = Total, 2, -1 do
        local j = UE.UKismetMathLibrary.RandomIntegerInRange(1, i)
        Controllers[i], Controllers[j] = Controllers[j], Controllers[i]
    end

    -- 发起变身选择：被选中者立即切为生化阵营并弹出选择框（从自己仓库中挑母体角色），
    -- 超时自动用保底角色。变身完成由 C++ 派发 OnGhostTransformed（见上方同名函数）。
    -- 同时在母体登记簿打标：完成时按"母体起步等级"落地（区别于战斗中被感染的 1 级起步）。
    for i = 1, MotherCount do
        self.MotherLedger[Controllers[i]] = true
        self:BeginGhostSelection(Controllers[i], CONFIG.GhostSelectionDuration)
    end

    print(string.format("[Match] 感染爆发：在场 %d 人，%d 名玩家进入母体变身选择（每 %d 人 1 名）",
        Total, MotherCount, CONFIG.MotherPerPlayers))
end

-- =============================================================================
-- 胜负判定与结算
-- =============================================================================

-- 判定"人类是否全灭"：是全灭则结算生化阵营胜；否则不做处理
-- @return 是否已触发结算
function GameMode:CheckAndResolveWinCondition()
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState or not IsCombatEnabledPhase(GameState:GetMatchPhase()) then
        return false
    end

    -- 人类全灭 → 生化阵营胜（本规则的主路径：最后一名人类被感染后即触发）
    if self:CountPlayersByFaction(UE.EGameFaction.Human) <= 0 then
        self:EndRound(CONFIG.ZombieTeamId, "人类全部被感染")
        return true
    end

    return false
end

-- 结束当前回合：结算比分并进入"结算展示"阶段（玩家重置推迟到结算倒计时结束）
-- @param WinningTeamId 获胜阵营 ID（对应 CONFIG.HumanTeamId / CONFIG.ZombieTeamId）
-- @param Reason        结束原因的日志描述（仅用于排障）
function GameMode:EndRound(WinningTeamId, Reason)
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    -- 1) 结算比分：胜方 +1（比分由 GameState 复制到全部客户端，供比分板展示）
    GameState:AddTeamScore(WinningTeamId, 1)

    local WinnerName = (WinningTeamId == CONFIG.HumanTeamId) and "人类" or "生化"
    print(string.format("[Match] 回合结束：%s 阵营获胜（%s）；当前比分 人类=%d 生化=%d",
        WinnerName, tostring(Reason),
        GameState:GetTeamScore(CONFIG.HumanTeamId), GameState:GetTeamScore(CONFIG.ZombieTeamId)))

    -- 2) 进入结算展示阶段：胜负已定但玩家暂不重置 ——
    --    阵亡者可观战、存活者可停留查看战场，结算面板（内容侧 WBP）由阶段变化事件弹出；
    --    玩家重置与下一局等待在结算倒计时结束后执行（见 OnMatchPhaseExpired 的 Settlement 分支）。
    --    副作用说明：本调用会替换掉当前阶段（Infection/Combat）的倒计时定时器，
    --                因此"提前全灭"后不会再收到旧阶段的到期事件。
    self:EnterPhase(UE.EGameMatchPhase.Settlement, CONFIG.SettlementDuration)
end

-- 阶段倒计时到期事件（订阅 GameState.OnGameMatchPhaseExpired）—— 回合循环的推进中枢
-- @param ExpiredPhase 刚刚到期的阶段；C++ 只维护单一定时器句柄，到期阶段必等于当时所处阶段
function GameMode:OnMatchPhaseExpired(ExpiredPhase)
    -- 仅服务器执行推进（客户端也收到广播，但阶段与比分写入只有服务器权威）
    if not self:HasAuthority() then
        return
    end

    if ExpiredPhase == UE.EGameMatchPhase.Warmup then
        -- 热身结束 → 进入感染阶段（选拔母体 + 变身选择窗口）
        self:BeginInfectionPhase()

    elseif ExpiredPhase == UE.EGameMatchPhase.Infection then
        -- 感染窗口结束 → 进入人类抵抗主战斗阶段（带回合倒计时）。
        -- 理论上母体变身早已完成（InfectionDuration ≥ GhostSelectionDuration + 缓冲）；
        -- 即便个别玩家仍在选择，其阵营已切生化、由 C++ 超时保底独立兜底，不影响阶段推进。
        self:EnterPhase(UE.EGameMatchPhase.Combat, CONFIG.RoundDuration)

    elseif ExpiredPhase == UE.EGameMatchPhase.Combat then
        -- 对局倒计时结束：仍有幸存人类 → 人类阵营胜；否则（理论上已被全感染）生化阵营胜
        if self:CountPlayersByFaction(UE.EGameFaction.Human) > 0 then
            self:EndRound(CONFIG.HumanTeamId, "对局时间结束且仍有幸存人类")
        else
            self:EndRound(CONFIG.ZombieTeamId, "对局时间内人类全部被感染")
        end

    elseif ExpiredPhase == UE.EGameMatchPhase.Settlement then
        -- 结算展示结束 → 重置全部玩家回人类，进入局间休整
        self:ResetAllPlayersToHuman()
        self:EnterPhase(UE.EGameMatchPhase.Intermission, CONFIG.IntermissionDuration)

    elseif ExpiredPhase == UE.EGameMatchPhase.Intermission then
        -- 休整结束：人数足够则自动开下一局（回热身），否则回大厅继续等待
        self:TryStartMatch()

    elseif ExpiredPhase == UE.EGameMatchPhase.Lobby then
        -- 大厅默认无倒计时，理论上不会到期；保留分支仅作兜底（如内容侧给大厅配了倒计时）
        self:TryStartMatch()
    end
end

-- =============================================================================
-- 查询/重置辅助（纯数据操作，不含规则）
-- =============================================================================

-- 统计当前处于指定阵营的玩家数量
function GameMode:CountPlayersByFaction(Faction)
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return 0
    end

    local Count = 0
    local Arr = GameState.PlayerArray
    for i = 1, Arr:Num() do
        local PS = Arr[i]
        if PS and PS:GetPlayerFaction() == Faction then
            Count = Count + 1
        end
    end
    return Count
end

-- 统计在场玩家总数
function GameMode:CountPlayers()
    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return 0
    end
    return GameState.PlayerArray:Num()
end

-- 把全部玩家重置回人类阵营（结算展示结束后使用）：
-- 一律执行重生（不做"只重生僵尸"的优化），原因有二：
--   ① 观察者（人类死亡后转观察者）的阵营仍是 Human，只按阵营判断会漏掉他们，导致跨回合卡在观战；
--   ② 回合重置本就该让所有人"满血、回出生点"，重生是最直接且语义正确的做法。
function GameMode:ResetAllPlayersToHuman()
    -- 作废母体登记簿与重选待办/进行中表：回合重置会清掉"未完成变身/未完成重选"的状态
    --（阵营复核使其作废），陈旧标记若不清理，下一回合会误发起步等级或误开重选窗口。
    self.MotherLedger = {}
    self.PendingReSelection = {}
    self.ReSelectionInFlight = {}

    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        return
    end

    local Arr = GameState.PlayerArray
    for i = 1, Arr:Num() do
        local PS = Arr[i]
        local PC = PS and PS:GetPlayerController()
        if PC then
            -- 按阵营重生为人类：内部完成阵营切换 + 属性集切换 + 新角色初始属性/技能授予。
            -- 对观察者玩家同样有效：引擎在重新附身时会自动把状态从 Spectating 切回 Playing。
            self:RespawnPlayerAsFaction(PC, UE.EGameFaction.Human, nil)
        end
    end
end

-- =============================================================================
-- GM 调试命令（服务器控制台入口："GK <命令> [参数...]"）
--   机制：UGameGMSubsystem 注册 GK 控制台命令（仅非 Shipping 构建）→ 解析出命令 ID 与参数串
--         → GameMode:DispatchGmCommand（服务器权威复核）→ 派发下方同名函数 OnGmCommand；
--   规则：全部命令行为就在本文件的 GM_COMMANDS 表里 —— 新增/修改命令只热更 Lua、不重编译 C++
--         （这正是"入口在 C++、行为在 Lua"的 GM 架构核心收益）。
--   目标约定：命令的最后一个字符串参数是目标玩家（玩家名子串，忽略大小写；
--         或 PlayerArray 的 1 基序号）；缺省时默认名单中**第一个玩家** ——
--         开发单机场景即主机本人，正好覆盖"自我感染/自我改级"的常用诉求。
-- =============================================================================

-- 把 C++ 传来的参数串按空白切分成参数数组
local function ParseGmArgs(Args)
    local Tokens = {}
    if Args and Args ~= "" then
        for Token in string.gmatch(Args, "%S+") do
            Tokens[#Tokens + 1] = Token
        end
    end
    return Tokens
end

-- 解析目标玩家：Token = 玩家名子串（忽略大小写）/ 1 基序号 / nil（默认第一个玩家）
-- @return PlayerState, Controller（找不到返回 nil, nil）
local function ResolveGmTarget(GameState, Token)
    local Arr = GameState.PlayerArray
    local Total = Arr:Num()
    if Total <= 0 then
        return nil, nil
    end

    if not Token or Token == "" then
        local PS = Arr[1]
        return PS, PS and PS:GetPlayerController()
    end

    -- 纯数字参数按"1 基序号"解释（玩家名恰好是数字的极端情况请用完整名匹配）
    local Index = tonumber(Token)
    if Index then
        Index = math.floor(Index) -- 防浮点入参（TArray 下标必须是整数）
        if Index >= 1 and Index <= Total then
            local PS = Arr[Index]
            return PS, PS and PS:GetPlayerController()
        end
        return nil, nil
    end

    local Needle = string.lower(Token)
    for i = 1, Total do
        local PS = Arr[i]
        local Name = PS and PS:GetPlayerName()
        if Name and string.find(string.lower(Name), Needle, 1, true) then
            return PS, PS:GetPlayerController()
        end
    end
    return nil, nil
end

-- 命令清单（Help 按此顺序打印；新增命令记得在这里登记一行）
local GM_COMMAND_ORDER = { "Help", "SkipPhase", "ForceStart", "Settle", "Infect", "SetLevel", "AddInfect", "LevelUp", "ClearStats" }

-- 命令行为表：Usage/Desc 供 GK.Help 展示；Run(self, GameState, Tokens) 是命令本体
local GM_COMMANDS = {}

GM_COMMANDS.Help = {
    Usage = "GK.Help",
    Desc = "列出全部 GM 命令",
    Run = function(self, GameState, Tokens)
        print("[GM] ===== GM 命令清单（行为表在 Lua，加命令不重编译）=====")
        for _, Name in ipairs(GM_COMMAND_ORDER) do
            local Command = GM_COMMANDS[Name]
            print(string.format("[GM]   %-26s %s", Command.Usage, Command.Desc))
        end
    end,
}

-- ------------------------- 对局流程类 -------------------------

GM_COMMANDS.SkipPhase = {
    Usage = "GK.SkipPhase",
    Desc = "跳到下一阶段（等价于本阶段倒计时到期）",
    Run = function(self, GameState, Tokens)
        -- 复用"倒计时到期"的推进规则：跳阶段 ≠ 绕过规则，行为与自然到期完全一致，
        -- 因此不需要（也不允许）在这里另写一套阶段推进逻辑
        self:OnMatchPhaseExpired(GameState:GetMatchPhase())
    end,
}

GM_COMMANDS.ForceStart = {
    Usage = "GK.ForceStart",
    Desc = "强制开局（绕过人数门槛；仅大厅/休整阶段有效）",
    Run = function(self, GameState, Tokens)
        local Phase = GameState:GetMatchPhase()
        if Phase ~= UE.EGameMatchPhase.Lobby and Phase ~= UE.EGameMatchPhase.Intermission then
            print("[GM] ForceStart：对局已在进行中，忽略")
            return
        end
        -- 直接走开局（跳过 TryStartMatch 的人数校验）：开发场景 1 人也能跑通全流程
        self:StartMatch()
    end,
}

GM_COMMANDS.Settle = {
    Usage = "GK.Settle",
    Desc = "立即结算（按当前存活判定胜方；仅感染/战斗阶段有效）",
    Run = function(self, GameState, Tokens)
        if not IsCombatEnabledPhase(GameState:GetMatchPhase()) then
            print("[GM] Settle：不在感染/战斗阶段，忽略")
            return
        end
        -- 与 Combat 倒计时到期同一判定口径：有幸存人类 → 人类胜，否则生化胜
        if self:CountPlayersByFaction(UE.EGameFaction.Human) > 0 then
            self:EndRound(CONFIG.HumanTeamId, "GM 强制结算")
        else
            self:EndRound(CONFIG.ZombieTeamId, "GM 强制结算")
        end
    end,
}

-- ------------------------- 感染进化类 -------------------------

GM_COMMANDS.Infect = {
    Usage = "GK.Infect [玩家]",
    Desc = "把目标玩家感染（走真实变身选择流程；1 级起步）",
    Run = function(self, GameState, Tokens)
        local PS, PC = ResolveGmTarget(GameState, Tokens[1])
        if not PC then
            print("[GM] Infect：找不到目标玩家")
            return
        end
        if PS:GetPlayerFaction() ~= UE.EGameFaction.Human then
            print("[GM] Infect：目标不是人类阵营（已是幽灵/观战中），忽略")
            return
        end
        -- 不打母体登记簿标记 → OnGhostTransformed 走"普通感染"分支（InfectedStartLevel 起步），
        -- 与"命中即感染"完全同链路（阵营即时切生化、弹选择框、超时保底），保证 GM 验证的就是真实规则
        if self:BeginGhostSelection(PC, CONFIG.GhostSelectionDuration) then
            print(string.format("[GM] Infect：%s 进入变身选择", PS:GetPlayerName()))
            self:CheckAndResolveWinCondition()
        end
    end,
}

GM_COMMANDS.SetLevel = {
    Usage = "GK.SetLevel <1-6> [玩家]",
    Desc = "设置幽灵进化等级（完整落地：血量增量/体型/模型）",
    Run = function(self, GameState, Tokens)
        local Level = tonumber(Tokens[1])
        if not Level or Level < 1 or Level > CONFIG.ZombieLevelMax then
            print(string.format("[GM] SetLevel：等级须为 1..%d", CONFIG.ZombieLevelMax))
            return
        end
        Level = math.floor(Level) -- int32 入参口径统一
        local PS, PC = ResolveGmTarget(GameState, Tokens[2])
        if not PS then
            print("[GM] SetLevel：找不到目标玩家")
            return
        end
        if PS:GetPlayerFaction() ~= UE.EGameFaction.Zombie then
            print("[GM] SetLevel：目标不是生化幽灵，忽略")
            return
        end
        local Pawn = PC and PC:GetPawn()
        if Pawn then
            -- 走角色入口：等级状态 + 血量增量 + 体型/模型完整落地。
            -- 已知取舍（调试工具可接受）：把等级"改低"时只改等级状态与表现，
            -- 已发放的血量增量不回收（C++ 增量施加是单向幂等；正式规则不存在"降级"，
            -- 只有"死亡重置 1 级"走重生全量重置路径）。
            Pawn:SetZombieLevel(Level)
        else
            PS:SetZombieLevel(Level) -- 无 Avatar（选择中/待复活）：只写状态，新 Avatar 附身时按等级摆好形态
        end
        print(string.format("[GM] SetLevel：%s → %d 级", PS:GetPlayerName(), Level))
    end,
}

GM_COMMANDS.AddInfect = {
    Usage = "GK.AddInfect <次数> [玩家]",
    Desc = "增加感染计数（原始状态修改；不自动触发升级）",
    Run = function(self, GameState, Tokens)
        local Count = math.floor(tonumber(Tokens[1]) or 1) -- int32 入参口径统一
        local PS = ResolveGmTarget(GameState, Tokens[2])
        if not PS then
            print("[GM] AddInfect：找不到目标玩家")
            return
        end
        PS:AddInfectionCount(Count)
        print(string.format("[GM] AddInfect：%s 感染计数 → %d", PS:GetPlayerName(), PS:GetRoundInfectionCount()))
    end,
}

GM_COMMANDS.LevelUp = {
    Usage = "GK.LevelUp [玩家]",
    Desc = "模拟升 1 级（等级 +1、计数清零；等价感染数达标）",
    Run = function(self, GameState, Tokens)
        local PS, PC = ResolveGmTarget(GameState, Tokens[1])
        if not PS then
            print("[GM] LevelUp：找不到目标玩家")
            return
        end
        if PS:GetPlayerFaction() ~= UE.EGameFaction.Zombie then
            print("[GM] LevelUp：目标不是生化幽灵，忽略")
            return
        end
        local Current = PS:GetZombieLevel()
        if Current >= CONFIG.ZombieLevelMax then
            print(string.format("[GM] LevelUp：%s 已是满级 %d", PS:GetPlayerName(), CONFIG.ZombieLevelMax))
            return
        end
        local Pawn = PC and PC:GetPawn()
        if Pawn then
            Pawn:SetZombieLevel(Current + 1)
        else
            PS:SetZombieLevel(Current + 1)
        end
        PS:SetRoundInfectionCount(0)
        print(string.format("[GM] LevelUp：%s → %d 级", PS:GetPlayerName(), Current + 1))
    end,
}

GM_COMMANDS.ClearStats = {
    Usage = "GK.ClearStats [玩家]",
    Desc = "清空回合战绩（K/D/伤害，含等级与感染计数归零）",
    Run = function(self, GameState, Tokens)
        local PS = ResolveGmTarget(GameState, Tokens[1])
        if not PS then
            print("[GM] ClearStats：找不到目标玩家")
            return
        end
        PS:ResetRoundStats()
        print(string.format("[GM] ClearStats：%s 战绩已清空", PS:GetPlayerName()))
    end,
}

-- GM 命令落地入口（由 C++ AGameGameMode.DispatchGmCommand 派发；服务器权威）
function GameMode:OnGmCommand(CommandId, Args)
    if not self:HasAuthority() then
        return
    end

    local Command = GM_COMMANDS[tostring(CommandId)]
    if not Command then
        print(string.format("[GM] 未知命令：GK.%s（输入 GK.Help 查看命令列表）", tostring(CommandId)))
        return
    end

    local GameState = UE.UGameplayStatics.GetGameState(self)
    if not GameState then
        print("[GM] 未取到 GameState，命令中止")
        return
    end

    Command.Run(self, GameState, ParseGmArgs(Args))
end

return GameMode
