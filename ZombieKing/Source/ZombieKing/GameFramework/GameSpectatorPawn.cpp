#include "GameSpectatorPawn.h"
#include "GameSpectatorComponent.h"   // 观战大脑（目标列表/切换/相机模式下发）
#include "GamePlayerController.h"     // 宿主控制器：取观战组件
#include "GameCharacterBase.h"        // 目标的复制瞄准朝向（ReplicatedAimRotation）
#include "InputAction.h"              // UInputAction 完整类型（BindAction 形参需要）
#include "InputActionValue.h"         // FInputActionValue
#include "InputMappingContext.h"      // UInputMappingContext
#include "EnhancedInputComponent.h"   // UEnhancedInputComponent
#include "EnhancedInputSubsystems.h"  // UEnhancedInputLocalPlayerSubsystem（激活 IMC）
#include "Camera/CameraComponent.h"   // UCameraComponent（取目标眼睛位置）
#include "Camera/CameraTypes.h"       // FMinimalViewInfo（CalcCamera 出参）
#include "GameFramework/FloatingPawnMovement.h" // UFloatingPawnMovement（飞行速度 MaxSpeed）
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Engine/CollisionProfile.h"  // UCollisionProfile（关闭碰撞用预置名）
#include "Engine/LocalPlayer.h"
#include "CollisionQueryParams.h"     // FCollisionQueryParams（第三人称防穿墙扫掠）
#include "CollisionShape.h"           // FCollisionShape
#include "Math/RotationMatrix.h"      // FRotationMatrix（按偏航展开前后/左右方向）
#include "ZombieKing.h"

AGameSpectatorPawn::AGameSpectatorPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// 刻意**不**创建任何相机组件：
	// 三种观战取景全部由覆写的 CalcCamera 自行计算（见 .cpp 下方），
	// 相机的"位置/朝向"是模式与目标的函数，用组件反而要绕一大圈（改挂点/改臂长/改旋转来源）。
	// 这样做的另一个好处：观战相机完全不依赖角色的骨骼动画与弹簧臂，行为在专用服务器/慢客户端上也稳定。

	// -------------------------------------------------------------------------
	// 1. 关闭碰撞：本项目规则 = 自由视角允许穿墙（观战/导播需要看清全图）
	// -------------------------------------------------------------------------
	// 注意：必须在**构造期**决定碰撞形态（引擎在组件注册后无法再安全地整体替换碰撞体形态，
	// 项目既有约定亦如此）。基类 ASpectatorPawn 使用的是 "Spectator" 预置碰撞档案，
	// 这里显式改成 NoCollision，使浮动飞行不会被墙体阻挡。
	// 第三人称的"防穿墙"与此无关：它由 CalcCamera 自己的球形扫掠负责（见下方）。
	if (UPrimitiveComponent* RootPrimitive = Cast<UPrimitiveComponent>(GetRootComponent()))
	{
		RootPrimitive->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}

	// 说明：飞行速度（MaxSpeed）刻意不在构造期写入 —— 内容侧可能在蓝图 Class Defaults 里
	//       改 FreeCamMaxSpeed，构造期的赋值会被随后的属性拷贝覆盖。统一改在 BeginPlay 施加。
}

void AGameSpectatorPawn::BeginPlay()
{
	Super::BeginPlay();

	// 施加自由视角飞行速度（放这里而非构造期，保证蓝图子类的配置值生效）
	if (UFloatingPawnMovement* FloatingMovement = Cast<UFloatingPawnMovement>(MovementComponent))
	{
		FloatingMovement->MaxSpeed = FreeCamMaxSpeed;
	}
}

// =============================================================================
//  输入绑定
// =============================================================================
void AGameSpectatorPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// 关键决策：内容侧配了观战 IMC 就走 Enhanced Input，并**关闭引擎默认轴映射**——
	// 否则同一按键会被"引擎 legacy 轴映射"和"Enhanced Input"各推动一次 → 双倍移动。
	// 未配置 IMC 时保留引擎默认轴映射，使观战在零内容配置下也能用 WASD 飞行（工程兜底）。
	const bool bUseEnhancedInput = (DefaultMappingContext != nullptr);
	if (bUseEnhancedInput)
	{
		bAddDefaultMovementBindings = false;
	}

	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (!bUseEnhancedInput)
	{
		UE_LOG(LogZombieKing, Log,
			TEXT("[%s] 观战 Pawn 未配置 DefaultMappingContext：已回落引擎默认轴映射"
			     "（WASD 可飞行，但无法绑定切目标/切相机模式按键）。"
			     "请在观战 Pawn 蓝图的 Class Defaults → Spectator|Input 下配置观战 IMC 与输入动作。"),
			*GetName());
		return;
	}

	// 激活观战专用 IMC（优先级 1，高于角色 IMC；观战时角色已不存在，这里取优先级只为表达"观战输入优先"）
	if (const APlayerController* OwningPC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSubsystem =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(OwningPC->GetLocalPlayer()))
		{
			InputSubsystem->AddMappingContext(DefaultMappingContext, /*Priority=*/1);
		}
	}

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		return;
	}

	// 自由视角驾驶（仅在 FreeCam 模式下生效，见各回调内的模式判断）
	if (ActionMove)
	{
		EnhancedInput->BindAction(ActionMove, ETriggerEvent::Triggered, this, &AGameSpectatorPawn::Input_Move);
	}
	if (ActionLook)
	{
		EnhancedInput->BindAction(ActionLook, ETriggerEvent::Triggered, this, &AGameSpectatorPawn::Input_Look);
	}
	if (ActionMoveUp)
	{
		EnhancedInput->BindAction(ActionMoveUp, ETriggerEvent::Triggered, this, &AGameSpectatorPawn::Input_MoveUp);
	}

	// 观战操作：切目标 / 切相机模式（可选通道，未配不绑定也不告警）
	if (ActionNextTarget)
	{
		EnhancedInput->BindAction(ActionNextTarget, ETriggerEvent::Started, this, &AGameSpectatorPawn::Input_NextTarget);
	}
	if (ActionPrevTarget)
	{
		EnhancedInput->BindAction(ActionPrevTarget, ETriggerEvent::Started, this, &AGameSpectatorPawn::Input_PrevTarget);
	}
	if (ActionCycleCameraMode)
	{
		EnhancedInput->BindAction(ActionCycleCameraMode, ETriggerEvent::Started, this, &AGameSpectatorPawn::Input_CycleCameraMode);
	}
}

// =============================================================================
//  相机模式 / 跟随目标（由 UGameSpectatorComponent 下发）
// =============================================================================
void AGameSpectatorPawn::SetCameraMode(EGameSpectatorCameraMode NewMode)
{
	// 哨兵值/重复设置直接忽略，避免无意义的事件广播
	if (NewMode == EGameSpectatorCameraMode::MAX || CameraMode == NewMode)
	{
		return;
	}

	CameraMode = NewMode;

	// 进入第三人称时把"环绕朝向"对齐到目标背后：
	// 否则相机会沿观察者上一次的任意朝向绕目标转，落点不可预期（可能正好卡在目标正面看到脸）。
	if (NewMode == EGameSpectatorCameraMode::FollowThirdPerson)
	{
		if (APawn* Target = FollowTarget.Get())
		{
			if (APlayerController* OwningPC = Cast<APlayerController>(GetController()))
			{
				OwningPC->SetControlRotation(ResolveFollowAimRotation(Target));
			}
		}
	}

	OnCameraModeChanged.Broadcast(NewMode);
}

void AGameSpectatorPawn::SetFollowTarget(APawn* NewTarget)
{
	// 弱引用保存：目标被销毁（阵亡/离场）时自动失效，跟随模式随即回退到自由视角取景，
	// 不需要在目标销毁时做任何解绑（避免"销毁时回调已失效对象"的一类崩溃）。
	FollowTarget = NewTarget;
}

FRotator AGameSpectatorPawn::GetFollowTargetViewRotation() const
{
	if (APawn* Target = FollowTarget.Get())
	{
		return ResolveFollowAimRotation(Target);
	}
	return GetControlRotation();
}

// =============================================================================
//  观战相机取景（三种模式）
// =============================================================================
void AGameSpectatorPawn::CalcCamera(float DeltaTime, FMinimalViewInfo& OutResult)
{
	// 刻意不调用 Super：
	// 引擎基类会去找相机组件或用 Actor 眼位，而观战取景的三模式全部由本项目自行计算。
	// 其余字段（FOV / 投影模式 / 后处理权重等）由 PlayerCameraManager 在调用本函数前
	// 已用 DefaultFOV 等默认值初始化（见 APlayerCameraManager::UpdateViewTarget），故这里只填位置/朝向。
	APawn* Target = FollowTarget.Get();

	switch (CameraMode)
	{
	case EGameSpectatorCameraMode::FollowFirstPerson:
		if (Target)
		{
			// 跟随·第一人称：位置取目标的眼睛位置，朝向取目标的瞄准朝向 —— 等价于"透过他的眼睛看"。
			OutResult.Location = ResolveFollowEyeLocation(Target);
			OutResult.Rotation = ResolveFollowAimRotation(Target);
			return;
		}
		break; // 无目标 → 落到自由视角兜底

	case EGameSpectatorCameraMode::FollowThirdPerson:
		if (Target)
		{
			// 跟随·第三人称：以目标眼睛为锚点，沿观察者环绕朝向的反方向拉出臂长。
			const FVector AnchorLocation = ResolveFollowEyeLocation(Target);
			const FRotator OrbitRotation = GetControlRotation();
			const FVector Backward = -OrbitRotation.Vector();
			FVector DesiredLocation = AnchorLocation + Backward * ThirdPersonArmLength;

			// 防穿墙扫掠：从锚点向期望机位扫一个球。
			// 之所以自己扫而不挂弹簧臂：弹簧臂要求"宿主组件被搬到目标身上"，会把观察者 Pawn
			// 的位置与目标强行绑死（带来移动组件/复制语义的混乱）；自己扫掠既无副作用又完全可控。
			FCollisionQueryParams QueryParams(FName(TEXT("GameSpectatorThirdPersonProbe")),
				/*bTraceComplex=*/false, /*InIgnoreActor=*/this);
			QueryParams.AddIgnoredActor(Target); // 目标自身的碰撞体不能挡住相机

			const FCollisionShape ProbeShape = FCollisionShape::MakeSphere(ThirdPersonProbeRadius);
			FHitResult ProbeHit;
			if (UWorld* World = GetWorld())
			{
				if (World->SweepSingleByChannel(ProbeHit, AnchorLocation, DesiredLocation, FQuat::Identity,
					ECC_Camera, ProbeShape, QueryParams))
				{
					// 撞到遮挡物：把机位贴到命中点并沿法线退开一个球半径（避免相机陷进墙里看到内壁）
					DesiredLocation = ProbeHit.Location + ProbeHit.ImpactNormal * (ThirdPersonProbeRadius + 2.0f);

					// 兜底：目标紧贴墙面时命中点可能离锚点极近，会导致相机怼在目标脸上 ——
					// 这种情况下退回到一个"最小安全距离"，宁可略微穿墙也不给出不可用的画面。
					const float MinSafeDistance = FMath::Min(ThirdPersonArmLength, 60.0f);
					if (FVector::DistSquared(AnchorLocation, DesiredLocation) < FMath::Square(MinSafeDistance))
					{
						DesiredLocation = AnchorLocation + Backward * MinSafeDistance;
					}
				}
			}

			OutResult.Location = DesiredLocation;
			OutResult.Rotation = OrbitRotation;
			return;
		}
		break; // 无目标 → 落到自由视角兜底

	case EGameSpectatorCameraMode::FreeCam:
	default:
		break;
	}

	// 自由视角（同时是"跟随模式但暂时无目标"的兜底）：
	// 相机就在观察者 Pawn 上，朝向 = 控制器朝向；因观察者 Pawn 已关闭碰撞，飞行可穿墙。
	OutResult.Location = GetActorLocation();
	OutResult.Rotation = GetControlRotation();
}

// =============================================================================
//  自由视角飞行（覆写引擎默认轴映射回调：非自由视角时彻底不响应）
// =============================================================================
void AGameSpectatorPawn::MoveForward(float Val)
{
	// 仅自由视角响应移动：跟随模式下观察者 Pawn 是否移动都不影响取景，
	// 但让移动组件继续跑会白费模拟开销，也容易让人误以为"移动了却看不到变化"。
	if (CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		return;
	}
	Super::MoveForward(Val);
}

void AGameSpectatorPawn::MoveRight(float Val)
{
	if (CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		return;
	}
	Super::MoveRight(Val);
}

void AGameSpectatorPawn::MoveUp_World(float Val)
{
	if (CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		return;
	}
	Super::MoveUp_World(Val);
}

// =============================================================================
//  Enhanced Input 回调
// =============================================================================
void AGameSpectatorPawn::Input_Move(const FInputActionValue& Value)
{
	if (CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		return;
	}

	const FVector2D MoveAxis = Value.Get<FVector2D>();

	// 只按"偏航"在水平面内平移：观战飞行不该因为抬头就朝天上冲（垂直交给 Input_MoveUp）。
	// 轴约定与引擎模板一致：Y = 前/后，X = 左/右。
	const FRotator FlatRotation(0.0f, GetControlRotation().Yaw, 0.0f);
	AddMovementInput(FlatRotation.Vector(), MoveAxis.Y);
	AddMovementInput(FRotationMatrix(FlatRotation).GetUnitAxis(EAxis::Y), MoveAxis.X);
}

void AGameSpectatorPawn::Input_Look(const FInputActionValue& Value)
{
	// 视角输入在三模式下的语义：
	//  - 自由视角：控制自己的观察方向；
	//  - 跟随·第三人称：绕被观战者环绕（相机朝向即环绕朝向）；
	//  - 跟随·第一人称：被忽略（取景完全由目标的眼睛位置与瞄准朝向决定，观察者不能乱看）。
	const FVector2D LookAxis = Value.Get<FVector2D>();
	AddControllerYawInput(LookAxis.X);
	AddControllerPitchInput(LookAxis.Y);
}

void AGameSpectatorPawn::Input_MoveUp(const FInputActionValue& Value)
{
	if (CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		return;
	}
	AddMovementInput(FVector::UpVector, Value.Get<float>());
}

void AGameSpectatorPawn::Input_NextTarget()
{
	// 观战操作一律转发给"观战大脑"，由它统一决定目标列表、合法性、事件广播 ——
	// 观战 Pawn 只负责驾驶与取景，不做任何决策（保持职责单一）。
	if (UGameSpectatorComponent* SpectatorComponent = GetSpectatorComponent())
	{
		SpectatorComponent->NextTarget();
	}
}

void AGameSpectatorPawn::Input_PrevTarget()
{
	if (UGameSpectatorComponent* SpectatorComponent = GetSpectatorComponent())
	{
		SpectatorComponent->PrevTarget();
	}
}

void AGameSpectatorPawn::Input_CycleCameraMode()
{
	if (UGameSpectatorComponent* SpectatorComponent = GetSpectatorComponent())
	{
		SpectatorComponent->CycleCameraMode();
	}
}

// =============================================================================
//  内部工具
// =============================================================================
UGameSpectatorComponent* AGameSpectatorPawn::GetSpectatorComponent() const
{
	// 观战组件挂在"宿主玩家控制器"上（相机管理是 PC 职责），因此必须经由控制器取用。
	if (const AGamePlayerController* GamePC = Cast<AGamePlayerController>(GetController()))
	{
		return GamePC->GetSpectatorComponent();
	}
	return nullptr;
}

FVector AGameSpectatorPawn::ResolveFollowEyeLocation(APawn* Target) const
{
	if (!Target)
	{
		return FVector::ZeroVector;
	}

	// 优先用目标身上的相机组件世界位置：那正是"该玩家自己看到的位置"。
	// 对远端玩家而言，其骨骼动画在观战端由复制的动画状态本地模拟，位置足够准确。
	if (const UCameraComponent* TargetCamera = Target->FindComponentByClass<UCameraComponent>())
	{
		return TargetCamera->GetComponentLocation();
	}

	// 兜底：目标没有相机组件（非本项目角色）时用引擎统一的眼位（Actor 位置 + BaseEyeHeight）
	return Target->GetPawnViewLocation();
}

FRotator AGameSpectatorPawn::ResolveFollowAimRotation(APawn* Target) const
{
	if (!Target)
	{
		return FRotator::ZeroRotator;
	}

	// 本端控制的 Pawn（例如 ListenServer 主机观战本机控制的角色）：控制旋转就是权威值
	if (Target->IsLocallyControlled())
	{
		return Target->GetControlRotation();
	}

	// 远端目标：用角色自己复制的瞄准朝向。
	// 为什么不能直接用 Controller->GetControlRotation()：AController::ControlRotation 是
	// 非复制字段（UPROPERTY() 无 replicat），在观战端那份 PC 上恒为 0，会得到完全错误的方向。
	if (const AGameCharacterBase* TargetCharacter = Cast<AGameCharacterBase>(Target))
	{
		const FRotator ReplicatedAim = TargetCharacter->GetReplicatedAimRotation();
		// 复制尚未到达（开局首帧）时该值为零：此时回退到 Actor 朝向 + 复制的俯仰，避免瞬间朝向乱跳。
		if (!ReplicatedAim.IsNearlyZero())
		{
			return ReplicatedAim;
		}
	}

	// 兜底：Actor 朝向 + 引擎复制的俯仰（APawn::RemoteViewPitch16，服务器在 PreReplication 写入）
	FRotator FallbackRotation = Target->GetActorRotation();
	FallbackRotation.Pitch = FRotator::DecompressAxisFromShort(Target->GetRemoteViewPitch());
	FallbackRotation.Roll = 0.0f;
	return FallbackRotation;
}
