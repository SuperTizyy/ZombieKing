#include "GameGameMode.h"
#include "GamePlayerState.h"      // 对局玩家状态（托管 GAS、阵营与技能）
#include "GamePlayerController.h" // 玩家控制器（本机 HUD 宿主 + 观战状态机入口）
#include "GameSpectatorPawn.h"    // 观察者 Pawn（观战相机载体）
#include "GameGameState.h"        // 对局全局状态（阶段 / 比分）
#include "GameCharacterBase.h"    // 角色基类（按阵营生成的 Avatar 类型约束）
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h" // APlayerController::ChangeState / ClientGotoState（观察者状态切换）
#include "Engine/World.h"         // GetWorld() / GetAuthGameMode / GetTimerManager（延迟复活计时）
#include "TimerManager.h"         // FTimerManager（延迟复活计时器）
#include "ZombieKing.h"           // LogZombieKing（生成失败告警）

AGameGameMode::AGameGameMode()
{
    // -------------------------------------------------------------------------
    // 1. 对局框架装配（GAS 闭环与阶段状态机的前提）
    // -------------------------------------------------------------------------
    // GameState：承载"对所有客户端可见"的权威对局信息（阶段 / 阶段时间 / 阵营比分）
    GameStateClass = AGameGameState::StaticClass();
    // PlayerState：托管 ASC、属性集与技能（跨死亡/变异存活，作为 GAS 的 OwnerActor）
    PlayerStateClass = AGamePlayerState::StaticClass();
    // PlayerController：本机玩家的 HUD 宿主（在本地创建/持有 HUD 控件），并为玩家级表现预留挂载点。
    // 之所以在 GameMode 装配而非依赖蓝图：保证换关卡/新建蓝图子类时不会漏配，框架三条类装配口径一致。
    PlayerControllerClass = AGamePlayerController::StaticClass();

    // SpectatorClass：进入观察者状态时由引擎按 GameState->SpectatorClass 生成观察者 Pawn
    // （见 APlayerController::SpawnSpectatorPawn；GameState 上的该字段由本 GameMode 复制过去）。
    // 必须指定为本项目的 AGameSpectatorPawn —— 否则引擎默认的 ASpectatorPawn 只有"飞行的空壳"，
    // 没有观战相机取景（自由/跟随·第一人称/跟随·第三人称）与观战输入，玩家会"进入观战却看不到东西"。
    SpectatorClass = AGameSpectatorPawn::StaticClass();

    // -------------------------------------------------------------------------
    // 2. Lua 规则脚本绑定
    // -------------------------------------------------------------------------
    // 阶段推进、开局条件、感染规则、胜负判定等"规则类"逻辑全部写在 Lua 中（可热更）；
    // 本 C++ 类只负责装配、事件派发与框架级动作。
    LuaScriptPath = TEXT("Lua.Core.GameFramework.GameMode");
}

FString AGameGameMode::GetModuleName_Implementation() const
{
    // 交由 UnLua 按此路径绑定规则脚本（路径相对 Content/Script，层级用 . 连接）
    return LuaScriptPath;
}

void AGameGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);

    // 只派发"框架级事件"：是否达到开局人数、是否切阶段等规则完全由 Lua 决定（可热更）
    OnPlayerEnteredMatch(NewPlayer);
}

void AGameGameMode::Logout(AController* Exiting)
{
    Super::Logout(Exiting);

    // 同理：人数不足/中途离场后的回退规则由 Lua 处理
    OnPlayerLeftMatch(Exiting);
}

UClass* AGameGameMode::GetDefaultPawnClassForController_Implementation(AController* InController)
{
    if (InController)
    {
        if (const AGamePlayerState* PlayerState = InController->GetPlayerState<AGamePlayerState>())
        {
            // 1) 显式指定优先：感染/换人时写入的"下次重生 Pawn 类"
            if (UClass* PendingPawnClass = PlayerState->GetPendingPawnClass().Get())
            {
                return PendingPawnClass;
            }

            // 2) 否则按"当前阵营的默认角色类"生成：
            //    玩家开局阵营默认为人类，因此首次入场即按人类角色生成，无需依赖任何入场时序。
            if (const TSubclassOf<AGameCharacterBase>* FactionPawnClass = FactionPawnClasses.Find(PlayerState->GetPlayerFaction()))
            {
                if (UClass* FactionClass = FactionPawnClass->Get())
                {
                    return FactionClass;
                }
            }
        }
    }

    // 3) 都没配置时回退到引擎常规默认（DefaultPawnClass），保证常规流程不受影响
    return Super::GetDefaultPawnClassForController_Implementation(InController);
}

bool AGameGameMode::RespawnPlayerAsFaction(AController* Controller, EGameFaction Faction,
                                           TSubclassOf<AGameCharacterBase> PawnClassOverride)
{
    // 服务器权威：只有服务器能决定角色的重新生成，客户端调用直接忽略（防作弊）
    if (!HasAuthority() || !Controller)
    {
        return false;
    }

    AGamePlayerState* PlayerState = Controller->GetPlayerState<AGamePlayerState>();
    if (!PlayerState)
    {
        return false;
    }

    // 1) 分配阵营：内部会切换已注册的属性集（人类 ↔ 生化）；客户端由复制与 OnRep 同步。
    //    属性集切换后数值从零起算，随后由新角色的初始化 GE 灌满。
    PlayerState->SetPlayerFaction(Faction);

    // 1b) 清除"观战/变身选择"标记：任何一次复活（感染变异重生 / 僵尸延迟复活 / 回合重置统一复活）
    //     都意味着该玩家重新参与战斗、不再是观察者，也不应停留在"正在挑选幽灵角色"的状态
    //     （变身选择期间被回合重置覆盖时，其 PC 上的超时定时器稍后触发会因标志已清而安全作废）。
    //     客户端侧的观战相机收尾不依赖这里 —— 它由 AGamePlayerController 在"重新附身"路径上自动完成
    //     （OnPossess / ClientRestart → HandleLocalSpectatorExit）。
    PlayerState->SetIsSpectating(false);
    PlayerState->SetAwaitingGhostSelection(false);

    // 2) 决定重生使用的 Pawn 类：显式传入优先，否则回退到该阵营配置的默认角色类
    TSubclassOf<AGameCharacterBase> ChosenPawnClass = PawnClassOverride;
    if (!ChosenPawnClass)
    {
        if (const TSubclassOf<AGameCharacterBase>* FactionPawnClass = FactionPawnClasses.Find(Faction))
        {
            ChosenPawnClass = *FactionPawnClass;
        }
    }
    // 写入 PlayerState（由 GetDefaultPawnClassForController 读取）；
    // 若最终为空，则生成时回退到引擎默认，不会因此失败。
    PlayerState->SetPendingPawnClass(TSubclassOf<APawn>(ChosenPawnClass.Get()));

    // 3) 卸下并销毁旧 Avatar：
    //    - 先 UnPossess：会触发 AGameCharacterBase::UnPossessed → 清除本角色授予的技能；
    //    - 再销毁 Pawn：避免旧 Avatar 残留在场景中（Controller::UnPossess 不会自动销毁 Pawn）。
    if (APawn* OldPawn = Controller->GetPawn())
    {
        Controller->UnPossess();
        OldPawn->Destroy();
    }

    // 4) 生成并附身新 Avatar：
    //    RestartPlayer → SpawnDefaultPawnFor → GetDefaultPawnClassForController 返回第 2 步决定的类；
    //    新 Avatar 附身时 AGameCharacterBase::SetupAbilitySystemContext 会按其角色定义
    //    灌入初始属性 GE 并授予技能集。
    RestartPlayer(Controller);

    const bool bSucceeded = (Controller->GetPawn() != nullptr);
    if (!bSucceeded)
    {
        // 生成失败属于框架级异常（出生点缺失、角色类为空等），显式告警便于定位
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 按阵营重生失败（Controller=%s，阵营=%d，PawnClass=%s）："
                 "请检查该阵营的角色类是否有效、出生点是否可用。"),
            *GetName(), *GetNameSafe(Controller), static_cast<int32>(Faction), *GetNameSafe(ChosenPawnClass.Get()));
    }

    return bSucceeded;
}

bool AGameGameMode::InfectPlayer(AController* Controller, TSubclassOf<AGameCharacterBase> ZombiePawnClass)
{
    // 感染 = 以"生化阵营"重生；保留显式指定僵尸类的能力，便于 Lua 规则决定"变成哪种幽灵"。
    return RespawnPlayerAsFaction(Controller, EGameFaction::Zombie, ZombiePawnClass);
}

bool AGameGameMode::BeginGhostSelection(AController* Controller, float DurationSeconds)
{
    // 服务器权威：变身选择由 Lua 规则在服务器发起，客户端调用直接忽略
    if (!HasAuthority() || !Controller)
    {
        return false;
    }

    AGamePlayerController* PlayerController = Cast<AGamePlayerController>(Controller);
    AGamePlayerState* PlayerState = Controller->GetPlayerState<AGamePlayerState>();
    if (!PlayerController || !PlayerState)
    {
        return false;
    }

    // 前置校验 1：必须当前为人类阵营（重复发起 / 已是幽灵再发起都拒绝）。
    // 开局选拔与"攻击命中感染"都只发生在人类身上，这条校验同时充当幂等闸门。
    if (PlayerState->GetPlayerFaction() != EGameFaction::Human)
    {
        return false;
    }

    // 前置校验 2：已在变身选择中（异常重复触发）幂等拒绝
    if (PlayerState->IsAwaitingGhostSelection())
    {
        return false;
    }

    // -------------------------------------------------------------------------
    // 1) 阵营立即切换为生化幽灵（规则链连续性）：
    //    胜负判定（人类全灭）/ 重复感染拦截 / 比分统计全部按阵营计算，
    //    若等玩家选完角色才切阵营，中间窗口内会出现"已被感染却还能被再感染"的规则漏洞。
    // -------------------------------------------------------------------------
    PlayerState->SetPlayerFaction(EGameFaction::Zombie);

    // 2) 卸下并销毁人类 Avatar：选择期间该玩家无 Avatar、不可操作（与感染语义一致）。
    //    与 SetPlayerAsSpectator 同款手法：UnPossess → Destroy（Destroy 帧末生效，本帧内
    //    的表现逻辑仍可安全访问该 Actor）。
    if (APawn* CurrentPawn = PlayerController->GetPawn())
    {
        PlayerController->UnPossess();
        CurrentPawn->Destroy();
    }

    // 3) 置位选择状态（复制给全网，HUD 显示"XX 正在选择变身"）
    PlayerState->SetAwaitingGhostSelection(true);

    // 4) 交给控制器状态机：武装服务器超时定时器 + 通知属主客户端弹选择框
    //    （玩家从仓库选 / 超时 → 服务器校验 → InfectPlayer 生成 → OnGhostTransformed 事件）
    PlayerController->BeginGhostSelectionServer(DurationSeconds);
    return true;
}

bool AGameGameMode::ReopenGhostSelection(AController* Controller, float DurationSeconds)
{
    // 服务器权威：重选窗口由 Lua 规则在复活完成后发起，客户端调用直接忽略
    if (!HasAuthority() || !Controller)
    {
        return false;
    }

    AGamePlayerController* PlayerController = Cast<AGamePlayerController>(Controller);
    AGamePlayerState* PlayerState = Controller->GetPlayerState<AGamePlayerState>();
    if (!PlayerController || !PlayerState)
    {
        return false;
    }

    // 前置校验 1：必须已是生化幽灵（与首次变身相反 —— 重选发生在幽灵复活之后；
    // 回合重置已把人切回人类的玩家会被这条自然拒绝）
    if (PlayerState->GetPlayerFaction() != EGameFaction::Zombie)
    {
        return false;
    }

    // 前置校验 2：已在任一选择窗口中（首次变身/上一次重选未结束）幂等拒绝
    if (PlayerState->IsAwaitingGhostSelection())
    {
        return false;
    }

    // 置位选择状态（复制给全网，HUD 显示"XX 正在重选幽灵"）；刻意**不卸 Avatar** ——
    // 重选是"机会窗口"：玩家带着当前角色照常行动，选完才换、不选拉倒（超时行为见状态机重选模式）。
    PlayerState->SetAwaitingGhostSelection(true);

    // 交给控制器状态机：武装超时定时器 + 通知属主客户端弹框（重选模式位 = true）
    PlayerController->BeginGhostSelectionServer(DurationSeconds, /*bReSelection=*/true);
    return true;
}

void AGameGameMode::ReportIncomingAttack(AController* VictimController, AActor* InstigatorActor, float DamageAmount)
{
    // 服务器权威：命中上报只在服务器受理 —— 客户端可伪造上报，受理即等于把"感染决定权"交给客户端（作弊面）。
    if (!HasAuthority() || !VictimController)
    {
        return;
    }

    // 只做"转发"：是否感染、感染成哪种幽灵、是否受数值门槛限制等规则一律由 Lua 判定。
    // 规则脚本实现 GameMode:OnIncomingAttackLanded(...) 即接管本事件（见 Content/Script/Lua/Core/GameFramework/GameMode.lua）。
    OnIncomingAttackLanded(VictimController, InstigatorActor, DamageAmount);
}

// =============================================================================
//  死亡处置（机制）
// =============================================================================

void AGameGameMode::ReportPlayerDeath(AController* VictimController, AActor* KillerActor)
{
    // 服务器权威：死亡处置会改变玩家状态与 Avatar，客户端不得参与
    if (!HasAuthority() || !VictimController)
    {
        return;
    }

    // 刻意"只派发事件、不移除角色"：
    // 同一帧内，致死命中还会触发"攻击命中即感染"的上报——引擎里 UHealthComponent 的死亡回调
    // 早于 UCombatComponent 的受击回调（两者都挂在同一条生命属性变化链上，按注册顺序广播），
    // 而感染链路的 Lua 需要通过角色读取其控制器。若在这里就 UnPossess，控制器会提前丢失，
    // 导致"人类被生化当场打死"本应变异为生化幽灵却静默失败。
    // 因此"把尸体移出战场"交由规则动作完成（两者都在同一帧内同步执行，不会有尸体残留）：
    //   - ScheduleRespawn：僵尸复活前先移除；
    //   - SetPlayerAsSpectator：人类转观察者时移除。
    OnPlayerDied(VictimController, KillerActor);
}

void AGameGameMode::ScheduleRespawn(AController* Controller, float DelaySeconds)
{
    if (!HasAuthority() || !Controller)
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 先把死亡 Avatar 移出战场（卸下 + 销毁）
    // 原因：延迟复活意味着该玩家在等待期内没有可操作角色；若尸体仍被控制，
    //       就会出现"尸体还能移动 / 开火 / 开技能"的荒谬情况。
    // 说明：Destroy() 在帧末生效，因此本帧内后续的表现逻辑仍能安全访问该 Actor。
    // -------------------------------------------------------------------------
    if (APawn* DeadPawn = Controller->GetPawn())
    {
        Controller->UnPossess();
        DeadPawn->Destroy();
    }

    FTimerManager& TimerManager = World->GetTimerManager();

    // 同一玩家若已有待复活计时（例如连续两次死亡上报），先取消旧计时，避免"复活两次"。
    if (FTimerHandle* ExistingHandle = PendingRespawnTimers.Find(Controller))
    {
        TimerManager.ClearTimer(*ExistingHandle);
    }

    if (DelaySeconds <= 0.0f)
    {
        // 立即复活：直接走同一套复活逻辑，无需计时器
        HandleScheduledRespawn(Controller);
        return;
    }

    // 计时器载荷用弱引用：等待期间玩家离场（Controller 销毁）时，回调里能安全识别并跳过。
    FTimerHandle& RespawnHandle = PendingRespawnTimers.Add(Controller);
    TimerManager.SetTimer(
        RespawnHandle,
        FTimerDelegate::CreateUObject(this, &AGameGameMode::HandleScheduledRespawn, TWeakObjectPtr<AController>(Controller)),
        DelaySeconds,
        /*bLoop=*/false);
}

void AGameGameMode::HandleScheduledRespawn(TWeakObjectPtr<AController> Controller)
{
    AController* RespawnController = Controller.Get();

    // 无论成功与否都清理句柄（一次性定时器已触发，句柄不再有意义）
    PendingRespawnTimers.Remove(Controller);

    if (!RespawnController)
    {
        // 玩家已离场：不再复活
        return;
    }

    // 若等待期间已被别处复活（例如回合重置统一复活了所有人），跳过以避免重复生成
    if (RespawnController->GetPawn())
    {
        return;
    }

    // 按"当前阵营"复活：僵尸被击杀后复活仍是僵尸（本项目死亡规则）
    EGameFaction Faction = EGameFaction::Human;
    if (const AGamePlayerState* PlayerState = RespawnController->GetPlayerState<AGamePlayerState>())
    {
        Faction = PlayerState->GetPlayerFaction();
    }

    RespawnPlayerAsFaction(RespawnController, Faction, /*PawnClassOverride=*/nullptr);
}

void AGameGameMode::SetPlayerAsSpectator(AController* Controller)
{
    if (!HasAuthority() || !Controller)
    {
        return;
    }

    AGamePlayerController* PlayerController = Cast<AGamePlayerController>(Controller);
    if (!PlayerController)
    {
        // AI 控制器没有观察者概念：无需处理
        return;
    }

    // 1) 卸下并销毁当前 Avatar：人类死亡后尸体不应继续留在战场
    if (APawn* CurrentPawn = PlayerController->GetPawn())
    {
        PlayerController->UnPossess();
        CurrentPawn->Destroy();
    }

    // 2) 标记"该玩家进入观战状态"（服务器权威 + 复制给全网）：
    //    这是"人"的状态，放 PlayerState；观战相机怎么取景是表现，放 PlayerController 的观战组件。
    if (AGamePlayerState* PlayerState = PlayerController->GetPlayerState<AGamePlayerState>())
    {
        PlayerState->SetIsSpectating(true);
    }

    // 3) 交给控制器进入观察者状态。
    //    控制器内部会：服务器侧切状态名 → 属主客户端生成观察者 Pawn（SpectatorClass=AGameSpectatorPawn）
    //    → 启动观战组件（目标列表/相机模式）→ 把相机混入观察者 Pawn。
    //    之所以把这段逻辑收敛到控制器而不是写在这里：相机/视图目标/观察者 Pawn 都是"控制器级"的概念，
    //    由控制器自己管理才不会出现"服务器切了状态、客户端却没有观战相机"的断层。
    PlayerController->EnterSpectatorMode();
}

// =============================================================================
//  GM 调试命令派发（机制：权威复核 + 转发；命令行为全在 Lua 的 GM_COMMANDS 表）
// =============================================================================

void AGameGameMode::DispatchGmCommand(const FString& CommandId, const FString& Args)
{
    // 服务器权威：GM 命令会改对局阶段与玩家状态，只允许服务器受理。
    // （GM 服务已保证只把命令打到权威 GameMode；此处复核是纵深防御，
    //   也让本接口可被其它服务器侧机制安全复用。）
    if (!HasAuthority())
    {
        return;
    }

    // 只派发：具体"命令做什么"由 Lua 规则脚本决定（GameMode.lua 的 OnGmCommand 同名接管）
    OnGmCommand(CommandId, Args);
}

void AGameGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 清除全部待复活计时器：对局销毁/关卡切换后若计时器仍存活，会回调到已失效的 GameMode 上
    if (UWorld* World = GetWorld())
    {
        FTimerManager& TimerManager = World->GetTimerManager();
        // 注意：ClearTimer 要求"非 const 的 FTimerHandle&"（它会复位句柄内部状态），
        // 因此这里必须用非 const 引用遍历；若写成 const 引用会因丢失限定符而编译失败。
        for (TPair<TWeakObjectPtr<AController>, FTimerHandle>& Pair : PendingRespawnTimers)
        {
            TimerManager.ClearTimer(Pair.Value);
        }
    }
    PendingRespawnTimers.Reset();

    Super::EndPlay(EndPlayReason);
}
