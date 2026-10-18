#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "GamePlayerController.generated.h"

class UGameHUDWidget;
class UGameSpectatorComponent;
class AGameCharacterBase;

/** 变身选择请求广播（本机客户端）：选择框 UI（WBP/Lua）据此弹出；倒计时秒数随事件给出 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameGhostSelectionRequested, float, DurationSeconds);

/**
 * 竞技生化网游：玩家控制器（本地 HUD 宿主 + 玩家级输入/表现挂载点 + 观战状态机入口）
 *
 * 职责边界（严格遵循热更新原则）：
 *  - 本类（C++）只做"机制"：为本机玩家创建 / 持有 / 销毁 HUD 控件，并保证它在
 *    Pawn 死亡与感染变异（Avatar 被销毁重建）期间长期存活；
 *  - HUD 的具体显示逻辑（血条动画、倒计时、比分板、提示横幅）全部在 HUD 控件脚本（Lua）中实现；
 *  - 本类不做任何对局规则判断（规则统一在 GameMode 绑定的 Lua 规则脚本里）。
 *
 * 为什么 HUD 挂在 PlayerController 而不是 Pawn：
 *  - Pawn（AGameCharacterBase）是短生命周期 Avatar：感染变异 / 死亡会被销毁并重建新 Pawn；
 *  - PlayerController 与 PlayerState 同寿命，HUD 挂在它上面才能在"变异重生"时保持连续、不闪断。
 *
 * 观战（本轮新增）：
 *  - 相机管理是 PlayerController 的天职，因此"观战状态机入口"（进入/退出引擎的观察者状态、
 *    生成/回收观察者 Pawn、驱动观战组件）落在本类；观战决策（看谁/用哪种相机）在
 *    UGameSpectatorComponent 里，取景计算在 AGameSpectatorPawn 里，三者职责严格分离。
 *  - 引擎事实（务必注意）：AController::ChangeState(NAME_Spectating) 只是改状态名，
 *    **不会**自动调用 BeginSpectatingState（引擎自身只在 ReceivedPlayer / ReceivedSpectatorClass
 *    两处自动调用）。因此进入观战必须由本类显式调用 BeginSpectatingState，
 *    否则会出现"状态名是观察者、却没有观察者 Pawn、没有观战相机"的空壳状态。
 *
 * 内容侧配置：在派生自本类的蓝图（BP_GamePlayerController）中指定 HUDWidgetClass
 * （一个派生自 UGameHUDWidget 的控件蓝图 WBP）；并把该蓝图设为 GameMode 的 PlayerControllerClass。
 */
UCLASS()
class ZOMBIEKING_API AGamePlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    AGamePlayerController();

    // -------------------------------------------------------------------------
    // HUD 控件访问
    // -------------------------------------------------------------------------
    /** 取本控制器持有的 HUD 控件（仅本机玩家会有；专用服务器与远端玩家为空） */
    UFUNCTION(BlueprintPure, Category = "HUD")
    UGameHUDWidget* GetHUDWidget() const { return HUDWidget; }

    // -------------------------------------------------------------------------
    // 观战访问
    // -------------------------------------------------------------------------
    /** 取本控制器的观战组件（观战决策中枢；服务器与客户端都持有，但只在客户端真正运作） */
    UFUNCTION(BlueprintPure, Category = "Spectator")
    UGameSpectatorComponent* GetSpectatorComponent() const { return SpectatorComponent; }

    /**
     * 服务器权威：让本控制器进入观察者状态。
     * 调用方：AGameGameMode::SetPlayerAsSpectator（由 Lua 死亡规则驱动）。
     * 内部动作：① 服务器侧把状态名切到 Spectating；② 属主客户端负责生成观察者 Pawn 并接管观战相机。
     * 注意：进入观战**不**代表"玩家不想玩了"——它只是"本回合不再参与战斗"；
     *       引擎的 PlayerState->bOnlySpectator 语义与此不同，本项目沿用 PlayerState 自己的 IsSpectating 标记。
     */
    UFUNCTION(BlueprintCallable, Category = "Spectator")
    void EnterSpectatorMode();

    /** 服务器权威：让本控制器退出观察者状态（回合重置时由复活流程的重新附身自动完成，一般无需显式调用） */
    UFUNCTION(BlueprintCallable, Category = "Spectator")
    void ExitSpectatorMode();

    // -------------------------------------------------------------------------
    // 变身选择（CF 式：变成幽灵时从仓库挑角色；状态权威在 PlayerState，流程状态机在此）
    // -------------------------------------------------------------------------
    /**
     * 变身选择请求事件（仅本机客户端触发）。
     * 选择框 UI（WBP/Lua）订阅后弹出选择界面；可选项从本机的幽灵仓库服务
     * （UGameGhostLoadoutSubsystem::GetOwnedGhosts）读取，倒计时秒数随事件给出；
     * 玩家选定后调用 SubmitGhostSelection(索引) 上报，超时由服务器自动用保底角色完成。
     * 说明：**幽灵重选窗口复用同一事件**（GameMode::ReopenGhostSelection 触发）——
     * 表现层无需区分首次变身/重选，弹框与提交完全同构；差异只在服务器侧的超时行为。
     */
    UPROPERTY(BlueprintAssignable, Category = "Ghost")
    FOnGameGhostSelectionRequested OnGhostSelectionRequested;

    /**
     * 上报玩家的选择（本机客户端调用；内部转发 Server RPC）。
     * @param GhostIndex 仓库清单中的索引（顺序与 UGameGhostLoadoutSubsystem::GetOwnedGhosts 一致，
     *                   也与服务器侧快照一致——快照就是本机上报的同一份清单）
     * 说明：索引而非类引用跨网上报，服务端校验"索引 ∈ 仓库快照"即完成防作弊（杜绝传任意类）。
     *       不在变身选择状态时调用会被安全忽略。
     */
    UFUNCTION(BlueprintCallable, Category = "Ghost")
    void SubmitGhostSelection(int32 GhostIndex);

    /**
     * 服务器：发起变身选择（由 AGameGameMode::BeginGhostSelection 在 Lua 规则触发后调用；
     * 供 GameMode 内部使用，但暴露为公开以便调试工具/蓝图复用）。
     * 内部：记录超时定时器 + 通知属主客户端弹框。
     * @param DurationSeconds 选择窗口倒计时（秒）
     * @param bReSelection    是否为"幽灵重选"模式（生化幽灵复活后换角色的窗口，由
     *                        AGameGameMode::ReopenGhostSelection 传入）：
     *                        两种模式的差异只在**超时行为** ——
     *                        首次变身超时 = 保底角色强制变身（玩家绝不能没有 Avatar）；
     *                        重选超时 = 放弃重选、保留当前角色（重选是"机会"不是"义务"）。
     *                        主动提交的选择两条链路完全一致（FinishGhostSelectionWithClass）。
     */
    void BeginGhostSelectionServer(float DurationSeconds, bool bReSelection = false);

    /**
     * 取消进行中的变身/重选窗口（仅服务器生效；BlueprintCallable 供 Lua 规则调用）。
     * 用途：选择窗口期间玩家死亡（刀杀禁复活/枪杀待复活）时，由 Lua 死亡规则先行取消，
     *       防止"窗口内迟到的提交把已转观战的玩家又拉回战场"这类状态错乱。
     * 行为：清超时定时器 + 清 PlayerState 待选标志 + 清重选模式位；不触发任何变身。
     */
    UFUNCTION(BlueprintCallable, Category = "Ghost")
    void CancelGhostSelectionServer();

    /** 本机是否正处于变身选择状态（查询便捷口；真值在 PlayerState 上） */
    UFUNCTION(BlueprintPure, Category = "Ghost")
    bool IsAwaitingGhostSelection() const;

protected:
    /** 生命周期：本机玩家创建 HUD 并加入视口 */
    virtual void BeginPlay() override;

    /** 生命周期：销毁 HUD，避免关卡切换 / 断开连接后控件残留 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // -------------------------------------------------------------------------
    // 观战状态机（引擎钩子）
    // 说明：BeginSpectatingState 是引擎"真正进入观察者状态"的入口（会生成并绑定观察者 Pawn、
    //       把相机视图目标指向它）；EndSpectatingState 是离开时的收尾钩子。
    //       两者在 APlayerController 中均为 protected virtual，故只能在本类内调用/覆写。
    // -------------------------------------------------------------------------
    virtual void BeginSpectatingState() override;
    virtual void EndSpectatingState() override;

    /**
     * 重新附身钩子（服务器/主机路径）：APlayerController::OnPossess 内部会把状态切回 Playing。
     * 在切状态之前本类先做"退出观战"的收尾（停观战组件、销毁观察者 Pawn），
     * 从而覆盖所有复活路径（感染变异、僵尸延迟复活、回合重置），无需在每处调用方手工收尾。
     */
    virtual void OnPossess(APawn* InPawn) override;

    /**
     * 客户端收到"重新附身"的通知（远端客户端路径）：客户端的附身不走 OnPossess，
     * 而是走 ClientRestart（内部 ChangeState(NAME_Playing)），因此这里同样需要收尾。
     */
    virtual void ClientRestart_Implementation(APawn* NewPawn) override;

    /** 本机收尾：停止观战组件并销毁本机的观察者 Pawn（幂等，可重复调用） */
    void HandleLocalSpectatorExit();

    /**
     * PlayerState 复制回调（客户端/服务器都会走）。
     * 用途：登录时序竞态的兜底 —— 客户端上报幽灵仓库快照的 Server RPC 可能早于 PlayerState
     * 复制到达服务器，服务器实现会把快照暂存；PlayerState 就绪（本回调）时补写。
     */
    virtual void OnRep_PlayerState() override;

    /** 本机执行"进入观战"：切状态名 → BeginSpectatingState → 启动观战组件 */
    void ApplyEnterSpectatorLocal();

    /** 本机执行"退出观战"：停观战组件 → 销毁观察者 Pawn → 状态切回 Playing */
    void ApplyExitSpectatorLocal();

    /**
     * 进入观战的客户端通知（Reliable Client RPC）。
     * 之所以要自己发这个 RPC 而不复用引擎的 ClientGotoState：ClientGotoState 内部只做 ChangeState，
     * 即上面提到的"只改状态名、不生成观察者 Pawn"的坑；本 RPC 里显式补上 BeginSpectatingState。
     */
    UFUNCTION(Client, Reliable)
    void ClientEnterSpectatorMode();

    /** 退出观战的客户端通知（Reliable Client RPC） */
    UFUNCTION(Client, Reliable)
    void ClientExitSpectatorMode();

    // -------------------------------------------------------------------------
    // 变身选择：网络入口（RPC 与服务器定时器；选择框 UI 只见上面的广播/BlueprintCallable）
    // -------------------------------------------------------------------------
    /**
     * 进入变身选择的客户端通知（Reliable Client RPC，携带倒计时秒数）。
     * 只做"广播 OnGhostSelectionRequested"一件事——选择框的弹出、可选项枚举（读本机仓库服务）、
     * 提交（SubmitGhostSelection）都由表现层接手。
     */
    UFUNCTION(Client, Reliable)
    void ClientNotifyGhostSelectionStarted(float DurationSeconds);

    /**
     * 上报变身选择的 Server RPC（Reliable：选择结果绝不能丢，丢了玩家会一直停在无 Avatar 状态直到超时）。
     * @param GhostIndex 仓库快照索引；服务器校验"索引 ∈ 快照"后按类完成变身（见 FinishGhostSelectionWithClass）
     */
    UFUNCTION(Server, Reliable)
    void ServerSubmitGhostSelection(int32 GhostIndex);

    /**
     * 服务器侧上报幽灵仓库快照的 Server RPC（Reliable）。
     * 由本机 BeginPlay 自动调用（读本机仓库服务整表上报），服务器存入 PlayerState 快照；
     * 之后"选择 ∈ 快照"的校验都以这份快照为准（见 AGamePlayerState::SetOwnedGhostClasses 的信任模型说明）。
     */
    UFUNCTION(Server, Reliable)
    void ServerSubmitGhostLoadout(const TArray<TSubclassOf<AGameCharacterBase>>& OwnedGhostClasses);

    /**
     * 变身选择超时回调（服务器）：自动用 GameMode 的保底角色（DefaultGhostClass）完成变身。
     * 与"玩家主动上报"汇合到同一条完成链路（FinishGhostSelectionWithClass）。
     */
    void HandleGhostSelectionTimeout();

    /**
     * 变身选择的统一完成链路（服务器）：校验状态与阵营 → 置位结束 → 调 GameMode::InfectPlayer 生成所选幽灵
     * → 派发 OnGhostTransformed 给 Lua 规则脚本做连锁。
     * @param GhostClass 已通过校验的所选角色类（调用方保证非空）
     */
    void FinishGhostSelectionWithClass(TSubclassOf<AGameCharacterBase> GhostClass);

    // -------------------------------------------------------------------------
    // 配置与运行时成员
    // -------------------------------------------------------------------------
    /**
     * HUD 控件类（内容侧指定，通常是一个派生自 UGameHUDWidget 的 WBP）。
     * 为空时不创建 HUD（便于专用服务器或纯逻辑测试跑通，不因缺资源而报错）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "HUD")
    TSubclassOf<UGameHUDWidget> HUDWidgetClass;

    /** 本地 HUD 控件实例（仅在本机创建；用强引用持有，保证其生命周期与本控制器一致） */
    UPROPERTY(Transient, BlueprintReadOnly, Category = "HUD")
    TObjectPtr<UGameHUDWidget> HUDWidget;

    /**
     * 观战组件（观战决策中枢：可观察目标列表 / 目标切换 / 相机模式）。
     * 刻意做成独立组件而不是把逻辑堆在 PC 上：PC 需要长期保持"薄宿主"（HUD + 输入 + 状态机），
     * 观战这套决策逻辑自成一体、有独立的配置项与事件，做成组件才能被 HUD/蓝图层干净地复用与替换。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spectator")
    TObjectPtr<UGameSpectatorComponent> SpectatorComponent;

    /**
     * 变身选择超时定时器（仅服务器使用；由 BeginGhostSelectionServer 武装，
     * 完成变身/回合重置取消时清理）。载荷用弱引用语义由 UObject 定时器天然保证。
     */
    FTimerHandle GhostSelectionTimerHandle;

    /**
     * 当前选择窗口是否为"幽灵重选"模式（仅服务器使用；BeginGhostSelectionServer 写入，
     * 完成/取消/超时后清零）。只影响超时行为：首次=强制保底变身，重选=放弃并保留当前角色。
     */
    bool bGhostSelectionIsReSelection = false;

    /**
     * 幽灵仓库快照的服务器侧暂存（仅服务器使用）。
     * 上报 RPC 早于 PlayerState 复制到达时先存这里，OnRep_PlayerState 时补写进 PlayerState。
     */
    UPROPERTY(Transient)
    TArray<TSubclassOf<AGameCharacterBase>> PendingGhostLoadoutReport;
};

