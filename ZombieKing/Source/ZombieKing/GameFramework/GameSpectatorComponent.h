#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameSpectatorTypes.h"
#include "GameSpectatorComponent.generated.h"

class APawn;
class APlayerState;
class AGamePlayerController;
class AGameSpectatorPawn;

/** 观战目标变化事件（参数为新的被观战 Pawn；无目标时为空） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameSpectatorTargetChanged, AActor*, NewTarget);

/** 观战相机模式变化事件 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameSpectatorCameraModeChanged, EGameSpectatorCameraMode, NewCameraMode);

/** 观战状态（进入/退出观战）变化事件：HUD 据此切换"观战 HUD / 战斗 HUD" */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameSpectatorStateChanged, bool, bSpectating);

/**
 * 竞技生化网游：观战组件（"观战大脑"，挂在 AGamePlayerController 上）
 *
 * 职责边界（与项目"PC 做宿主、能力下沉到组件"的既有分层一致）：
 *  - 本组件负责**决策**：谁能被观察（可观察目标列表）、当前观察谁、用哪种相机模式；
 *  - AGameSpectatorPawn 负责**执行**：按收到的模式与目标去计算相机取景、驾驶自由视角；
 *  - AGamePlayerController 负责**状态机**：按服务器指令进入/退出引擎的观察者状态并驱动本组件。
 *
 * 为什么观战决策不能放服务器（服务器权威原则的例外说明）：
 *  - "选谁来看、用什么机位看"是**每个观看者自己的体验选择**，不改变任何权威世界状态，
 *    也不是可被利用的作弊面（相机只影响本机画面，改不了别人看到的任何东西）；
 *  - 若把它做成服务器权威（客户端发请求、服务器裁决再下发），只会白白引入一个 RTT 的输入延迟，
 *    在竞技观战中手感会明显发粘。故本组件**只在本机客户端运行**（Tick 里也做了本机校验）。
 *  - 真正需要服务器权威的是"这个玩家是否处于观战状态"—— 那是 AGamePlayerState::bIsSpectating（复制字段），
 *    因为它会影响回合重置、复活、比分板等权威规则。
 *
 * 可观察目标列表的口径（首批规则）：
 *  - 取 GameState 的 PlayerArray（对局玩家名册），排除"自己"；
 *  - 只收**当前拥有有效 Pawn** 的玩家 —— 阵亡/已转观察者/尚未复活的玩家没有 Pawn，自然被排除，
 *    因此列表天然只包含"活着的人"，不需要再查血量（血量口径由服务器权威，客户端不必也不该自己判）。
 *  - 列表每次切换/巡检时重建，保证"中途有人复活/离场"都能即时反映。
 *
 * 内容侧配置：全部走 EditDefaultsOnly（改数值/默认模式不必重新编译 C++）。
 */
UCLASS(ClassGroup = (ZombieKing), meta = (BlueprintSpawnableComponent))
class ZOMBIEKING_API UGameSpectatorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UGameSpectatorComponent();

	// -------------------------------------------------------------------------
	// 查询接口（BlueprintPure：C++ / 蓝图 / Lua 均可调用）
	// -------------------------------------------------------------------------
	/** 本机是否正处于观战状态 */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	bool IsSpectating() const { return bSpectating; }

	/** 当前相机模式 */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	EGameSpectatorCameraMode GetCameraMode() const { return CameraMode; }

	/** 当前被观战的目标（无目标时为空） */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	AActor* GetCurrentTarget() const;

	/** 当前目标在可观察列表中的索引（无目标时为 -1） */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	int32 GetCurrentTargetIndex() const { return CurrentTargetIndex; }

	/** 可观察目标列表的快照（仅供 HUD/调试展示；内部缓存以弱引用保存） */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	TArray<AActor*> GetSpectatableTargets() const;

	// -------------------------------------------------------------------------
	// 观战生命周期（由 AGamePlayerController 在进入/退出观察者状态时调用）
	// -------------------------------------------------------------------------
	/**
	 * 开始观战（本机客户端）。
	 * 幂等：重复调用不会重复初始化。内部会重建目标列表并选一个默认目标/默认相机模式。
	 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void StartSpectating();

	/** 结束观战（本机客户端）。幂等；调用后相机视图目标应交还引擎的常规流程（重新附身时自动完成）。 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void StopSpectating();

	// -------------------------------------------------------------------------
	// 观战操作（本地玩家自身操作，不参与服务器权威）
	// -------------------------------------------------------------------------
	/** 切到下一个可观察目标 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void NextTarget();

	/** 切到上一个可观察目标 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void PrevTarget();

	/** 直接指定目标索引（越界会被夹取；传 INDEX_NONE / -1 表示无目标） */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void SetTargetIndex(int32 NewIndex);

	/** 设置相机模式（哨兵值与重复设置会被忽略） */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void SetCameraMode(EGameSpectatorCameraMode NewMode);

	/** 循环切换相机模式：自由 → 跟随·第一人称 → 跟随·第三人称 → 自由 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void CycleCameraMode();

	// -------------------------------------------------------------------------
	// 事件（供 HUD（WBP/Lua）订阅刷新"正在观战 X / 观战 HUD"）
	// -------------------------------------------------------------------------
	/** 观战目标变化 */
	UPROPERTY(BlueprintAssignable, Category = "Spectator")
	FOnGameSpectatorTargetChanged OnTargetChanged;

	/** 相机模式变化 */
	UPROPERTY(BlueprintAssignable, Category = "Spectator")
	FOnGameSpectatorCameraModeChanged OnCameraModeChanged;

	/** 观战状态（进入/退出）变化 */
	UPROPERTY(BlueprintAssignable, Category = "Spectator")
	FOnGameSpectatorStateChanged OnSpectatorStateChanged;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	// -------------------------------------------------------------------------
	// 内部实现
	// -------------------------------------------------------------------------
	/** 重建"可观察目标列表"（按 GameState 名册 + 有效 Pawn 过滤，排除自己） */
	void RebuildTargetList();

	/** 把"当前相机模式 + 当前目标"下发给观察者 Pawn（服务器无观察者 Pawn 时自动空操作） */
	void ApplyToSpectatorPawn();

	/** 设置当前目标索引（夹取到合法范围）并按需广播目标变化事件 */
	void SetCurrentTargetIndexInternal(int32 NewIndex, bool bBroadcast);

	/** 取列表索引对应的目标 Pawn（越界/已失效返回空） */
	APawn* GetTargetPawnAt(int32 Index) const;

	/** 按步骤环形切换目标（+1 下一个 / -1 上一个） */
	void StepTarget(int32 Step);

	/**
	 * 按观战状态切换音频混音（机械跟随；bSwitchAudioStateOnSpectate 关闭时为空操作）。
	 * 进入观战 → Spectate 混音；退出观战 → Match 混音（见 StopSpectating 内的理由说明）。
	 * 命名注意：参数用 bEnterSpectating，避开本类成员 bSpectating（重名触发 C4458）。
	 */
	void ApplySpectatorAudioState(bool bEnterSpectating);

	/** 取宿主玩家控制器 */
	AGamePlayerController* GetGamePlayerController() const;

	/** 取当前的观察者 Pawn（仅本机客户端存在；服务器/非观战端为空） */
	AGameSpectatorPawn* GetSpectatorPawn() const;

	// -------------------------------------------------------------------------
	// 配置（内容侧可调）
	// -------------------------------------------------------------------------
	/** 进入观战时相机混入观察者 Pawn 的时长（秒）：0 = 硬切 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator", meta = (ClampMin = "0"))
	float ViewTargetBlendSeconds = 0.4f;

	/**
	 * 选到目标后默认使用的相机模式（仅跟随类模式有意义）。
	 * 默认取"跟随·第一人称"：与 CF 生化模式死亡后跟随队友的第一人称视角惯例一致。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator")
	EGameSpectatorCameraMode DefaultFollowCameraMode = EGameSpectatorCameraMode::FollowFirstPerson;

	/** 目标有效性巡检间隔（秒）：用于"当前目标阵亡/离场后自动接管到下一名玩家" */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator", meta = (ClampMin = "0.05"))
	float TargetValidationInterval = 0.25f;

	/**
	 * 进入/退出观战时是否自动切换音频混音状态（Spectate ↔ Match）。
	 *
	 * 定位（大厂做法，机械跟随而非规则）：观战时切到"观战混音"几乎是必然的审美选择，
	 * 与前端流程的混音自动跟随（菜单/对局）同口径 —— 属于"状态 → 混音"的机械映射，
	 * 不含可调规则数值，因此放 C++ 默认跟随，零内容配置即生效；
	 * 开关留给未来特殊模式（例如某玩法要求观战保留战斗混音）由内容侧关闭后自行接管。
	 * 具体混音资产在 UGameAudioSettings 的 SpectateSoundMix（缺配时音频服务安全降级，无声但不报错崩）。
	 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Audio")
	bool bSwitchAudioStateOnSpectate = true;

	// -------------------------------------------------------------------------
	// 运行时状态（仅本机有意义，不参与复制）
	// -------------------------------------------------------------------------
	/** 是否处于观战状态 */
	bool bSpectating = false;

	/** 当前相机模式 */
	EGameSpectatorCameraMode CameraMode = EGameSpectatorCameraMode::FreeCam;

	/** 当前目标在可观察列表中的索引（-1 = 无目标；此时跟随模式退化为自由视角） */
	int32 CurrentTargetIndex = INDEX_NONE;

	/**
	 * 可观察目标列表缓存（弱引用）。
	 * 用弱引用而不是强引用：目标 Pawn 随时可能被销毁（阵亡/换阵营/离场），
	 * 强引用会让观战组件"吊住"已阵亡的 Pawn（延后其 GC），弱引用则让失效自然发生、
	 * 由巡检自动把失效目标替换掉。
	 */
	TArray<TWeakObjectPtr<APawn>> SpectatableTargets;
};
