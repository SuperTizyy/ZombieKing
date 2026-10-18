#include "GamePlayerController.h"
#include "GameHUDWidget.h"              // HUD 控件基类（内容侧以 WBP 派生）
#include "GameSpectatorComponent.h"     // 观战决策中枢（目标列表/切换/相机模式）
#include "GameSpectatorPawn.h"          // 观察者 Pawn（对齐"进入观战前的相机位姿"）
#include "GameGhostLoadoutSubsystem.h"  // 幽灵仓库服务（本机仓库清单来源）
#include "GamePlayerState.h"            // 玩家状态（仓库快照与变身选择状态的权威落点）
#include "GameGameMode.h"               // 对局 GameMode（变身完成调 InfectPlayer + Lua 连锁事件）
#include "GameCharacterBase.h"          // 幽灵角色基类（变身选择的目标类型）
#include "Blueprint/UserWidget.h"       // CreateWidget / RemoveFromParent
#include "GameFramework/Controller.h"   // ChangeState / NAME_Spectating / NAME_Playing
#include "Engine/World.h"               // GetWorld / GetTimerManager（选择超时定时器）
#include "TimerManager.h"               // FTimerManager（变身选择超时定时器）
#include "ZombieKing.h"                 // LogZombieKing（配置缺失告警）

AGamePlayerController::AGamePlayerController()
{
    // 刻意不改动 Tick 设置：
    // APlayerController 的 PlayerTick（输入处理、相机更新等）依赖 Tick，擅自关闭会破坏控制器功能；
    // HUD 的刷新本身也不需要控制器逐帧参与——它由"事件驱动（属性/阶段/比分的委托）+ 控件自身 Tick"完成。

    // 观战组件：构造期装配，与 GameMode/PlayerState 的装配口径一致（不依赖蓝图配置，避免漏配）。
    // 挂 PlayerController 而不是 Pawn：观战发生在"没有 Pawn"的时候，组件必须活在比 Pawn 更长的对象上。
    SpectatorComponent = CreateDefaultSubobject<UGameSpectatorComponent>(TEXT("SpectatorComponent"));
}

void AGamePlayerController::BeginPlay()
{
    Super::BeginPlay();

    // 只为"本机玩家"创建 HUD：
    //  - 专用服务器（Dedicated Server）没有本地视口，IsLocalController() 为 false；
    //  - 远端玩家的 HUD 在其各自的客户端上创建，不需要我们代劳（否则会把别人的血条画到自己屏幕上）。
    if (!IsLocalController())
    {
        return;
    }

    // 未配置控件类时不创建（便于纯逻辑测试或专用服务器安全跑通），仅给一条告警便于排查
    if (!HUDWidgetClass)
    {
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 未配置 HUDWidgetClass，本机 HUD 不会被创建。"
                 "请在派生自 AGamePlayerController 的蓝图默认值中指定一个派生自 UGameHUDWidget 的控件蓝图。"),
            *GetName());
    }
    else
    {
        // 创建并加入视口：Owner 传本控制器，使控件内的 GetOwningPlayer() 能拿回本控制器，
        // 进而定位到 PlayerState / GameState 作为数据源。
        HUDWidget = CreateWidget<UGameHUDWidget>(this, HUDWidgetClass);
        if (HUDWidget)
        {
            HUDWidget->AddToViewport();
        }
    }

    // 说明：若"进入对局前就处于观战"（例如中途加入的纯观察者），引擎会在 ReceivedPlayer /
    // ReceivedSpectatorClass 两处自动调用 BeginSpectatingState，本类的覆写随后接管观战相机，
    // 因此这里不需要额外补一次。

    // -------------------------------------------------------------------------
    // 幽灵仓库上报（仅本机客户端）：进对局即把本机仓库清单整表上报服务器。
    //  - 服务器把它存进 PlayerState 快照，之后"变身选择 ∈ 快照"的校验都以它为准；
    //  - PlayerState 可能尚未复制到达（登录时序竞态）：RPC 实现里会暂存并在
    //    OnRep_PlayerState 时补写（见 ServerSubmitGhostLoadout_Implementation）；
    //  - 专用服务器 / 非本机控制器走不到这里（上面已 return）。
    // -------------------------------------------------------------------------
    if (UGameGhostLoadoutSubsystem* GhostLoadout = GetGameInstance()->GetSubsystem<UGameGhostLoadoutSubsystem>())
    {
        ServerSubmitGhostLoadout(GhostLoadout->GetOwnedGhosts());
    }
}

void AGamePlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 清理变身选择超时定时器：控制器销毁后回调不得再进来
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GhostSelectionTimerHandle);
    }

    // 移除并释放 HUD，避免关卡切换 / PIE 结束时控件残留在视口中
    if (HUDWidget)
    {
        HUDWidget->RemoveFromParent();
        HUDWidget = nullptr;
    }

    Super::EndPlay(EndPlayReason);
}

// =============================================================================
//  观战：进入 / 退出（服务器权威入口）
// =============================================================================
void AGamePlayerController::EnterSpectatorMode()
{
    // 服务器权威：只有服务器能决定"某玩家转入观察者"。
    // 客户端调用会被静默忽略（防止客户端自行绕过"死亡才转观察者"这类回合规则）。
    if (!HasAuthority())
    {
        return;
    }

    // 服务器本端先把状态名切到观察者，供服务器侧逻辑（规则脚本/生成判定）查询 IsInState(NAME_Spectating)。
    // 注意：这一步只改状态名，不会生成观察者 Pawn —— 生成动作在客户端侧的 ApplyEnterSpectatorLocal 里。
    ChangeState(NAME_Spectating);

    if (IsLocalController())
    {
        // ListenServer 主机：直接把"进入观战"本机执行，比给自己发一个 Client RPC 更明确，
        // 也避免在部分网络模式下 RPC 投递给自己造成的重复执行。
        ApplyEnterSpectatorLocal();
    }
    else
    {
        // 远端客户端：走 Reliable Client RPC，由属主客户端生成观察者 Pawn 并接管观战相机。
        ClientEnterSpectatorMode();
    }
}

void AGamePlayerController::ExitSpectatorMode()
{
    if (!HasAuthority())
    {
        return;
    }

    if (IsLocalController())
    {
        ApplyExitSpectatorLocal();
    }
    else
    {
        ClientExitSpectatorMode();
    }
}

void AGamePlayerController::ClientEnterSpectatorMode_Implementation()
{
    ApplyEnterSpectatorLocal();
}

void AGamePlayerController::ClientExitSpectatorMode_Implementation()
{
    ApplyExitSpectatorLocal();
}

void AGamePlayerController::ApplyEnterSpectatorLocal()
{
    // 幂等保护：已经处于观察者状态且观察者 Pawn 已就绪时不再重复（否则会白白重建一次 Pawn、
    // 并重复做一次相机混入，观感上会闪一下）。
    if (IsInState(NAME_Spectating) && GetSpectatorPawn() != nullptr)
    {
        return;
    }

    // 状态名先对齐（服务器侧已在 EnterSpectatorMode 里切过；这里覆盖"客户端被 RPC 通知"的路径）
    if (!IsInState(NAME_Spectating))
    {
        ChangeState(NAME_Spectating);
    }

    // 记录"进入观战前"的相机位姿：
    // 死亡流程此前可能已把镜头切到击杀者（复仇镜头），这个位姿正是玩家死亡瞬间的最后画面。
    // 记录它的用途见下方（引擎生成观察者 Pawn 时会把它放到世界原点，必须修正）。
    FVector PreSpectateViewLocation = FVector::ZeroVector;
    FRotator PreSpectateViewRotation = FRotator::ZeroRotator;
    GetPlayerViewPoint(PreSpectateViewLocation, PreSpectateViewRotation);

    // 临时关闭引擎的"自动管理相机视图目标"（见 APlayerController::AutoManageActiveCameraTarget，
    // 其内部以 bAutoManageActiveCameraTarget 为闸门）：
    // 否则 BeginSpectatingState → SetSpectatorPawn → AutoManageActiveCameraTarget 会**硬切**到观察者 Pawn，
    // 之后观战组件再想用 SetViewTargetWithBlend 做平滑混入时，就已经没有"从哪个视图混过来"的源了（同一目标=空操作）。
    // 关闭后由观战组件用 0.4s 混入完成"死亡镜头 → 观战镜头"的转场，观感明显更好。
    // 关闭是**临时**的：本函数结束前立即恢复，避免影响后续重新附身时的相机自动接管。
    const bool bPreviousAutoManageCameraTarget = bAutoManageActiveCameraTarget;
    bAutoManageActiveCameraTarget = false;

    // 引擎标准入口：按 GameState->SpectatorClass 生成观察者 Pawn，把它挂到本控制器上，
    // 并（在自动管理开启时）把相机视图目标指向它（见 APlayerController::BeginSpectatingState）。
    // 这一步正是"有观察者状态名、却没有观战相机"这一常见坑的解法（见类头注释）。
    // 本类覆写的 BeginSpectatingState 会在其中顺带启动观战组件（由它负责带混入地接管相机）。
    BeginSpectatingState();

    // 恢复相机自动管理（此后重新附身时，引擎仍会自动把视图目标切回新 Pawn）
    bAutoManageActiveCameraTarget = bPreviousAutoManageCameraTarget;

    // 把观察者 Pawn 与控制器朝向对齐到"进入观战前的相机位姿"。
    // 为什么必须修正：引擎在 SpawnSpectatorPawn 里用的是 APlayerController::GetSpawnLocation()，
    // 而此刻控制器既没有 Pawn（已被卸下）也没有观察者 Pawn，它会退化为世界原点 ——
    // 不修正的话，自由视角会从 (0,0,0) 开始，玩家会经历一次莫名其妙的"传送"。
    // 命名注意：基类 APlayerController 有 protected 成员 SpectatorPawn，本变量刻意避开该名字
    //（否则触发 C4458"声明隐藏了类成员"）。
    if (AGameSpectatorPawn* SpawnedSpectatorPawn = Cast<AGameSpectatorPawn>(GetSpectatorPawn()))
    {
        // 只保留偏航：观察者 Pawn 应保持"站立"，保留俯仰会让它的根组件倾斜
        //（自由视角的取景用的是控制旋转，本身不受影响，这里只是让 Pawn 的姿态干净）。
        SpawnedSpectatorPawn->SetActorLocationAndRotation(
            PreSpectateViewLocation, FRotator(0.0f, PreSpectateViewRotation.Yaw, 0.0f));
        SetControlRotation(PreSpectateViewRotation);
    }
}

void AGamePlayerController::ApplyExitSpectatorLocal()
{
    if (SpectatorComponent)
    {
        SpectatorComponent->StopSpectating();
    }

    // 销毁本机的观察者 Pawn：它由引擎以 RF_Transient 生成（不进存档），但不会自动回收，
    // 不主动清理会一直漂在世界里（虽无功能危害，但会干扰调试与观测）。
    if (GetSpectatorPawn())
    {
        DestroySpectatorPawn();
    }

    // 状态切回 Playing：这是引擎语义上的"退出观战"。
    // 注意 AController::ChangeState 不会调用 EndSpectatingState，故上面的观战收尾是手工完成的。
    ChangeState(NAME_Playing);
}

// =============================================================================
//  观战：引擎状态机钩子
// =============================================================================
void AGamePlayerController::BeginSpectatingState()
{
    // 先让引擎完成标准动作：生成并绑定观察者 Pawn、把相机视图目标指向它
    Super::BeginSpectatingState();

    // 只有本机客户端需要真正的观战相机（远端/专用服务器没有本地视口，也不会生成观察者 Pawn）
    if (IsLocalController() && SpectatorComponent)
    {
        SpectatorComponent->StartSpectating();
    }
}

void AGamePlayerController::EndSpectatingState()
{
    // 本机收尾。
    // 说明：引擎不会在 ChangeState(NAME_Playing) 时调用本函数（见类头注释），
    //       实际收尾走 ApplyExitSpectatorLocal / HandleLocalSpectatorExit；
    //       这里再停一次是为了覆盖"引擎将来从其它路径调用 EndSpectatingState"的可能，
    //       且 StopSpectating 幂等，重复调用无副作用。
    if (IsLocalController() && SpectatorComponent)
    {
        SpectatorComponent->StopSpectating();
    }

    Super::EndSpectatingState();
}

void AGamePlayerController::HandleLocalSpectatorExit()
{
    // 幂等收尾：不在观战状态时什么都不做。
    // 本函数会被 OnPossess 与 ClientRestart 两条路径各调用一次（主机上还可能两条都触发），必须幂等。
    if (!IsInState(NAME_Spectating))
    {
        return;
    }

    if (SpectatorComponent)
    {
        SpectatorComponent->StopSpectating();
    }

    // 只有本机客户端持有观察者 Pawn；服务器上的远端 PC 从未生成过观察者 Pawn，跳过销毁即可。
    if (IsLocalController() && GetSpectatorPawn())
    {
        DestroySpectatorPawn();
    }

    // 把状态名切回 Playing：这是"退出观战"的引擎语义。
    ChangeState(NAME_Playing);
}

void AGamePlayerController::OnPossess(APawn* InPawn)
{
    // 服务器/主机路径：APlayerController::OnPossess 内部会 ChangeState(NAME_Playing)。
    // 在它把状态切走**之前**先做观战收尾（此刻 StateName 仍是 Spectating，能被正确识别）。
    // 这样就能用一条路径覆盖所有"复活"来源：感染变异重生、僵尸延迟复活、回合重置统一复活。
    HandleLocalSpectatorExit();

    Super::OnPossess(InPawn);
}

void AGamePlayerController::ClientRestart_Implementation(APawn* NewPawn)
{
    // 远端客户端路径：客户端上的附身不经过 OnPossess（那是权威路径），
    // 而是由服务器发来的 ClientRestart RPC 驱动（其内部会 ChangeState(NAME_Playing)）。
    // 因此客户端上的观战收尾必须挂在这里，否则客户端会一直停在观战相机上看不到自己的新角色。
    HandleLocalSpectatorExit();

    Super::ClientRestart_Implementation(NewPawn);
}

// =============================================================================
//  变身选择（CF 式：变成幽灵时从仓库挑角色）
//  流程总览（机制在 C++、规则在 Lua）：
//    Lua 规则（开局选拔 / 攻击命中感染）→ GameMode::BeginGhostSelection
//    → [服务器] PlayerState 置位 + 卸下人类 Avatar → 本函数族的 RPC 链
//    → [客户端] 弹选择框（OnGhostSelectionRequested，Lua/WBP 接手）
//    → 玩家选定 SubmitGhostSelection(索引) 或 服务器超时
//    → FinishGhostSelectionWithClass → GameMode::InfectPlayer 生成所选幽灵
//    → GameMode::OnGhostTransformed 事件回给 Lua 做连锁。
// =============================================================================
void AGamePlayerController::BeginGhostSelectionServer(float DurationSeconds, bool bReSelection)
{
    // 仅服务器：由 GameMode::BeginGhostSelection / ReopenGhostSelection（Lua 规则入口）在权威端调用
    if (!HasAuthority())
    {
        return;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // 幂等：已在选择中（连续两次死亡上报等）则重置超时定时器即可，不重复通知客户端
    if (IsAwaitingGhostSelection())
    {
        World->GetTimerManager().ClearTimer(GhostSelectionTimerHandle);
    }

    // 记录本次窗口的模式（决定超时行为，见头文件注释）；完成/取消链路负责清零
    bGhostSelectionIsReSelection = bReSelection;

    // 武装服务器超时定时器：到点按模式结算（首次变身=保底强制；重选=放弃保留，见 HandleGhostSelectionTimeout）
    World->GetTimerManager().SetTimer(GhostSelectionTimerHandle, this,
        &AGamePlayerController::HandleGhostSelectionTimeout, FMath::Max(DurationSeconds, 0.1f), /*bLoop=*/false);

    // 通知属主客户端弹出选择框（Reliable：通知绝不能丢，否则玩家会干等到超时）
    ClientNotifyGhostSelectionStarted(DurationSeconds);
}

void AGamePlayerController::CancelGhostSelectionServer()
{
    // 仅服务器（BlueprintCallable 供 Lua 死亡规则调用；客户端调用被安全忽略）
    if (!HasAuthority())
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GhostSelectionTimerHandle);
    }

    // 清重选模式位 + 清待选标志（标志内部同值短路，非窗口期调用无副作用）
    bGhostSelectionIsReSelection = false;
    if (AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>())
    {
        GhostPlayerState->SetAwaitingGhostSelection(false);
    }
}

void AGamePlayerController::ClientNotifyGhostSelectionStarted_Implementation(float DurationSeconds)
{
    // 只做"通知表现层"一件事：选择框的弹出/枚举/提交全部由 Lua/WBP 接手
    // （可选项读本机幽灵仓库服务；索引语义与上报快照一致，见 SubmitGhostSelection 注释）。
    OnGhostSelectionRequested.Broadcast(DurationSeconds);
}

void AGamePlayerController::SubmitGhostSelection(int32 GhostIndex)
{
    // 客户端本地预检：不在选择状态时静默忽略（选择框已关闭后迟到的点击等）
    if (!IsAwaitingGhostSelection())
    {
        return;
    }

    // 转发 Server RPC（Reliable）：真正校验与变身在服务器完成
    ServerSubmitGhostSelection(GhostIndex);
}

void AGamePlayerController::ServerSubmitGhostSelection_Implementation(int32 GhostIndex)
{
    // 权威预检 1：必须确实处于变身选择状态（防伪造 RPC 在任意时刻触发变身）
    if (!IsAwaitingGhostSelection())
    {
        return;
    }

    const AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>();
    if (!GhostPlayerState)
    {
        return;
    }

    // 权威预检 2：索引必须落在仓库快照内（"选择 ∈ 快照"防作弊校验；杜绝传任意类）
    const TArray<TSubclassOf<AGameCharacterBase>>& OwnedGhosts = GhostPlayerState->GetOwnedGhostClasses();
    if (!OwnedGhosts.IsValidIndex(GhostIndex))
    {
        // 索引非法（伪造或清单时序错位）：不打断流程，按超时保底处理 —— 玩家至少能正常变身
        UE_LOG(LogZombieKing, Warning,
            TEXT("[变身选择] 收到非法选择索引 %d（仓库快照 %d 项，Controller=%s），按保底角色完成变身"),
            GhostIndex, OwnedGhosts.Num(), *GetName());
        HandleGhostSelectionTimeout();
        return;
    }

    // 统一完成链路
    FinishGhostSelectionWithClass(OwnedGhosts[GhostIndex]);
}

void AGamePlayerController::ServerSubmitGhostLoadout_Implementation(
    const TArray<TSubclassOf<AGameCharacterBase>>& OwnedGhostClasses)
{
    AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>();
    if (!GhostPlayerState)
    {
        // 登录时序竞态：客户端上报早于 PlayerState 复制到达 —— 暂存，等 OnRep_PlayerState 补写
        PendingGhostLoadoutReport = OwnedGhostClasses;
        return;
    }

    GhostPlayerState->SetOwnedGhostClasses(OwnedGhostClasses);
}

void AGamePlayerController::OnRep_PlayerState()
{
    Super::OnRep_PlayerState();

    // 补写早到的仓库上报（见 ServerSubmitGhostLoadout_Implementation 的暂存说明）。
    // 注意：本回调两端都会走，但暂存只在服务器发生（客户端上恒为空），因此实际只有服务器补写。
    if (PendingGhostLoadoutReport.Num() > 0)
    {
        if (AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>())
        {
            GhostPlayerState->SetOwnedGhostClasses(PendingGhostLoadoutReport);
            PendingGhostLoadoutReport.Reset();
        }
    }
}

void AGamePlayerController::HandleGhostSelectionTimeout()
{
    // 幽灵重选模式的超时 = 玩家放弃重选：**保留当前角色继续战斗**（重选是机会不是义务），
    // 只做收尾（清标志/清模式位），绝不触发变身。
    if (bGhostSelectionIsReSelection)
    {
        bGhostSelectionIsReSelection = false;
        if (AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>())
        {
            GhostPlayerState->SetAwaitingGhostSelection(false);
        }
        return;
    }

    // 首次变身的超时 = 玩家未选（或上报非法）：用 GameMode 配置的保底角色完成变身，保证流程永不卡死
    const AGameGameMode* AuthGameMode = Cast<AGameGameMode>(GetWorld()->GetAuthGameMode());
    TSubclassOf<AGameCharacterBase> DefaultGhostClass = AuthGameMode ? AuthGameMode->GetDefaultGhostClass() : nullptr;

    if (!DefaultGhostClass)
    {
        // 保底类未配置：回退仓库快照第一项（基础幽灵通常在首位）；再不行只能放弃并告警
        if (const AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>())
        {
            const TArray<TSubclassOf<AGameCharacterBase>>& OwnedGhosts = GhostPlayerState->GetOwnedGhostClasses();
            if (OwnedGhosts.Num() > 0)
            {
                DefaultGhostClass = OwnedGhosts[0];
            }
        }
    }

    if (!DefaultGhostClass)
    {
        UE_LOG(LogZombieKing, Warning,
            TEXT("[变身选择] 超时完成失败：GameMode 未配置 DefaultGhostClass 且仓库快照为空（Controller=%s）。"
                 "请在 GameMode 蓝图（或其默认值）中配置保底幽灵角色类。"), *GetName());
        return;
    }

    FinishGhostSelectionWithClass(DefaultGhostClass);
}

void AGamePlayerController::FinishGhostSelectionWithClass(TSubclassOf<AGameCharacterBase> GhostClass)
{
    // 清定时器：无论从哪条路径完成，超时定时器都不再有意义
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(GhostSelectionTimerHandle);
    }

    // 窗口结束即清重选模式位（完成链路对首次/重选同构，之后行为不再依赖模式）
    bGhostSelectionIsReSelection = false;

    AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>();
    if (!HasAuthority() || !GhostPlayerState || !GhostPlayerState->IsAwaitingGhostSelection())
    {
        // 幂等保护：不在选择状态时（重复上报 / 超时与上报竞态）直接忽略
        return;
    }

    // 阵营复核：变身必须发生在"已是生化幽灵阵营"的前提下。
    // 若期间回合重置把该玩家切回了人类（RespawnPlayerAsFaction），本次选择作废 ——
    // 否则会出现"重置成人类后又被旧定时器变成僵尸"的规则错乱。
    if (GhostPlayerState->GetPlayerFaction() != EGameFaction::Zombie)
    {
        GhostPlayerState->SetAwaitingGhostSelection(false);
        return;
    }

    // 结束选择状态（先清标志再变身：变身内部复活流程还会复核一遍，双保险）
    GhostPlayerState->SetAwaitingGhostSelection(false);

    AGameGameMode* AuthGameMode = Cast<AGameGameMode>(GetWorld()->GetAuthGameMode());
    if (!AuthGameMode)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[变身选择] 完成失败：取不到权威 GameMode（Controller=%s）"), *GetName());
        return;
    }

    // 生成所选幽灵：走既有 InfectPlayer 链路（阵营/属性集已就绪，此处只负责生成 Avatar 并灌入数值技能）
    if (AuthGameMode->InfectPlayer(this, GhostClass))
    {
        // Lua 规则连锁（播报/特效/音效等）：实现 OnGhostTransformed 即接管
        AuthGameMode->OnGhostTransformed(this, GhostClass);
    }
}

bool AGamePlayerController::IsAwaitingGhostSelection() const
{
    const AGamePlayerState* GhostPlayerState = GetPlayerState<AGamePlayerState>();
    return GhostPlayerState && GhostPlayerState->IsAwaitingGhostSelection();
}
