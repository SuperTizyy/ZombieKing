#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "GameSpectatorTypes.h"
#include "GameSpectatorPawn.generated.h"

class UInputAction;
class UInputMappingContext;
class UGameSpectatorComponent;
class AGameCharacterBase;
struct FInputActionValue;

/** 观战 Pawn 相机模式变化事件（表现层可据此切 HUD 提示/相机转场音效） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameSpectatorPawnCameraModeChanged, EGameSpectatorCameraMode, NewCameraMode);

/**
 * 竞技生化网游：观察者 Pawn（观战相机载体）
 *
 * 为什么派生引擎的 ASpectatorPawn（而不是另起炉灶）：
 *  - 引擎的观察者状态机（APlayerController::BeginSpectatingState）会按 GameMode 的
 *    SpectatorClass 生成观察者 Pawn，并自动把它挂到控制器上、设为相机视图目标
 *    （见 APlayerController::SetSpectatorPawn → AttachToPawn + AutoManageActiveCameraTarget）。
 *    复用这条既有链路，我们只需覆写"相机怎么算"与"输入怎么用"两件事；
 *  - ASpectatorPawn 自带 USpectatorPawnMovement（无重力的浮动飞行）与引擎默认 WASD 轴映射，
 *    因此**零内容配置也能飞**（兜底路径），符合"内容侧未就绪也不崩、不空转"的工程要求。
 *
 * 本类职责（只做"相机与驾驶"这一层）：
 *  - 三种相机模式的取景计算（覆写 CalcCamera）：自由视角 / 跟随·第一人称 / 跟随·第三人称；
 *  - 自由视角的飞行驾驶（Enhanced Input，未配置时回落引擎默认轴映射）；
 *  - 观战操作输入（切目标 / 切相机模式）转发给 PC 上的 UGameSpectatorComponent。
 *
 * 明确不归本类管（避免职责越界）：
 *  - "哪些人可以被观察、当前观察谁、当前用哪种相机模式" —— 全部由 UGameSpectatorComponent 决定，
 *    本类只是被动执行它下发的 SetCameraMode / SetFollowTarget；
 *  - "谁进入了观战状态" —— 那是服务器权威状态（AGamePlayerState::bIsSpectating）与控制器的事。
 *
 * 内容侧配置：建一个派生自本类的蓝图（BP_GameSpectatorPawn），在 Class Defaults 里指定观战用的
 * IMC 与各输入动作，并把该蓝图设为 AGameGameMode 的 SpectatorClass（或在 GameMode 蓝图里覆盖）。
 */
UCLASS()
class ZOMBIEKING_API AGameSpectatorPawn : public ASpectatorPawn
{
	GENERATED_BODY()

public:
	// 说明：基类 ASpectatorPawn 由 GENERATED_UCLASS_BODY 声明、其构造函数要求显式传 FObjectInitializer，
	//       因此这里沿用项目既有写法（同 AGameCharacterBase）声明带默认参数的构造函数。
	AGameSpectatorPawn(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	// -------------------------------------------------------------------------
	// 相机状态（由 UGameSpectatorComponent 驱动；观战 Pawn 自身不做决策）
	// -------------------------------------------------------------------------
	/** 设置相机模式（切换瞬间会做必要的初始化，例如第三人称的环绕朝向对齐） */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void SetCameraMode(EGameSpectatorCameraMode NewMode);

	/** 当前相机模式 */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	EGameSpectatorCameraMode GetCameraMode() const { return CameraMode; }

	/**
	 * 设置被跟随的目标 Pawn（跟随模式下的观察对象）。
	 * 传 nullptr 表示无目标：跟随模式会自动退化为自由视角取景（见 CalcCamera），不会出现黑屏。
	 */
	UFUNCTION(BlueprintCallable, Category = "Spectator")
	void SetFollowTarget(APawn* NewTarget);

	/** 当前被跟随的目标（无目标时为空） */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	APawn* GetFollowTarget() const { return FollowTarget.Get(); }

	/**
	 * 取"当前跟随目标"的瞄准朝向（无目标时回退到本 Pawn 的控制旋转）。
	 * 供 UGameSpectatorComponent 在切到第三人称时把环绕朝向对齐到目标背后使用。
	 */
	UFUNCTION(BlueprintPure, Category = "Spectator")
	FRotator GetFollowTargetViewRotation() const;

	// -------------------------------------------------------------------------
	// 输入事件契约（供 Lua/蓝图订阅；本 Pawn 的 CameraMode/Target 变化都在上游组件里）
	// -------------------------------------------------------------------------
	/** 相框模式变化事件（表现层可据此切 HUD 提示） */
	UPROPERTY(BlueprintAssignable, Category = "Spectator")
	FOnGameSpectatorPawnCameraModeChanged OnCameraModeChanged;

protected:
	virtual void BeginPlay() override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	/**
	 * 相机取景总入口（引擎在把本 Pawn 当作视图目标时，每帧调用它计算 POV）。
	 * 我们在这里实现三种观战相机模式；这也是"观战相机"唯一权威的取景来源。
	 */
	virtual void CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult) override;

	// -------------------------------------------------------------------------
	// 自由视角飞行：覆写引擎默认轴映射回调，让"非自由视角"时彻底不响应移动
	// （跟随模式下移动观察者 Pawn 没有意义，且会白白消耗移动模拟开销）
	// -------------------------------------------------------------------------
	virtual void MoveForward(float Val) override;
	virtual void MoveRight(float Val) override;
	virtual void MoveUp_World(float Val) override;

	// -------------------------------------------------------------------------
	// Enhanced Input 回调（自由视角驾驶 + 观战操作）
	// -------------------------------------------------------------------------
	/** 水平移动（自由视角）：按"控制器偏航"在水平面内平移 */
	void Input_Move(const FInputActionValue& Value);

	/** 视角输入：驱动控制器旋转（自由视角=自己看的方向；第三人称=绕目标环绕） */
	void Input_Look(const FInputActionValue& Value);

	/** 垂直升降（自由视角）：沿世界 Z 轴上/下 */
	void Input_MoveUp(const FInputActionValue& Value);

	/** 切换到下一个可观察目标 */
	void Input_NextTarget();

	/** 切换到上一个可观察目标 */
	void Input_PrevTarget();

	/** 循环切换相机模式（自由 → 第一人称 → 第三人称 → 自由） */
	void Input_CycleCameraMode();

	// -------------------------------------------------------------------------
	// 内部工具
	// -------------------------------------------------------------------------
	/** 取宿主控制器的观战组件（观战大脑：目标列表/切换/模式） */
	UGameSpectatorComponent* GetSpectatorComponent() const;

	/**
	 * 解析被跟随目标的"眼睛位置"。
	 * 优先取目标身上相机组件的世界位置 —— 那正是该玩家自己看到的位置，
	 * "跟随第一人称"因此与"他本人所见"严格一致；目标没有相机组件时回退引擎眼位。
	 */
	FVector ResolveFollowEyeLocation(APawn* Target) const;

	/**
	 * 解析被跟随目标的"瞄准朝向"。
	 * 关键：远端目标**不能**用 Controller->GetControlRotation（AController::ControlRotation 不复制，
	 * 在观战端恒为 0），必须用角色自己复制的瞄准朝向（AGameCharacterBase::ReplicatedAimRotation）；
	 * 非本项目的 Pawn 则回退到"Actor 朝向 + 引擎复制的 RemoteViewPitch"。
	 */
	FRotator ResolveFollowAimRotation(APawn* Target) const;

	// -------------------------------------------------------------------------
	// 配置：Enhanced Input（内容侧在观战 Pawn 蓝图里指定）
	// 说明：全部走 EditDefaultsOnly，改按键/换 IMC 不必重新编译 C++。
	// -------------------------------------------------------------------------
	/** 观战专用输入映射上下文（为空则回落引擎默认 WASD 轴映射，保证零配置也能飞） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/** 水平移动（二维轴：X=左右, Y=前后） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionMove;

	/** 视角（二维轴：X=偏航, Y=俯仰） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionLook;

	/** 垂直升降（一维轴） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionMoveUp;

	/** 下一个目标（数字/按键，可选：不配则只能用 Lua/其它入口切换） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionNextTarget;

	/** 上一个目标（可选） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionPrevTarget;

	/** 循环切换相机模式（可选） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Input")
	TObjectPtr<UInputAction> ActionCycleCameraMode;

	// -------------------------------------------------------------------------
	// 配置：相机参数（内容侧可调，改数值不必重编译）
	// -------------------------------------------------------------------------
	/** 自由视角飞行最大速度（cm/s）；写入浮动移动组件的 MaxSpeed */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Camera", meta = (ClampMin = "0"))
	float FreeCamMaxSpeed = 1600.0f;

	/** 第三人称相机到目标的距离（cm） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Camera", meta = (ClampMin = "0"))
	float ThirdPersonArmLength = 320.0f;

	/** 第三人称防穿墙扫掠的球体半径（cm）：越大越不容易"贴墙看到墙体内部" */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Spectator|Camera", meta = (ClampMin = "0"))
	float ThirdPersonProbeRadius = 12.0f;

	// -------------------------------------------------------------------------
	// 运行时状态（由观测组件下发）
	// -------------------------------------------------------------------------
	/** 当前相机模式 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Spectator")
	EGameSpectatorCameraMode CameraMode = EGameSpectatorCameraMode::FreeCam;

	/** 当前被跟随的目标（弱引用：目标阵亡被销毁时自动失效，不需要手工解绑） */
	TWeakObjectPtr<APawn> FollowTarget;
};
