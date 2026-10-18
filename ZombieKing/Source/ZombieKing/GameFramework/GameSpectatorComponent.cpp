#include "GameSpectatorComponent.h"
#include "GameSpectatorPawn.h"     // 观察者 Pawn（接收相机模式与目标）
#include "GamePlayerController.h"  // 宿主控制器
#include "GameAudioSubsystem.h"    // 音频服务（观战混音的机械跟随：Spectate ↔ Match）
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "ZombieKing.h"

UGameSpectatorComponent::UGameSpectatorComponent()
{
	// 低频率巡检需求（目标阵亡后自动接管），因此开启 Tick，实际间隔在 BeginPlay 里按配置设置。
	PrimaryComponentTick.bCanEverTick = true;

	// 观战组件不参与复制：
	// "当前看谁、用哪种相机"纯属本机表现，服务器与其它客户端既不需要也不应该关心
	// （真需要复制的是"此玩家是否处于观战状态"——那在 AGamePlayerState 上）。
	SetIsReplicatedByDefault(false);
}

void UGameSpectatorComponent::BeginPlay()
{
	Super::BeginPlay();

	// 以"定时巡检"而非逐帧 Tick 来做目标有效性校验：
	// 目标阵亡/离场不是高频事件，0.25s 的粒度足以让切换在观感上"即时"，同时几乎零开销。
	PrimaryComponentTick.TickInterval = FMath::Max(TargetValidationInterval, 0.05f);
}

void UGameSpectatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 对局销毁/断线/关卡切换时收尾：回到非观战状态并清空目标缓存，
	// 避免残留状态在同一个 PC 被复用（如无缝切图）时造成"看着已失效的目标"。
	if (bSpectating)
	{
		StopSpectating();
	}

	Super::EndPlay(EndPlayReason);
}

// =============================================================================
//  观战生命周期
// =============================================================================
void UGameSpectatorComponent::StartSpectating()
{
	// 幂等：正常流程下 AGamePlayerController 只会调一次，但热重载/异常路径可能重复触发
	if (bSpectating)
	{
		return;
	}
	bSpectating = true;

	// 重建目标列表并选默认目标：列表非空取第一个；空则不选目标
	RebuildTargetList();
	CurrentTargetIndex = (SpectatableTargets.Num() > 0) ? 0 : INDEX_NONE;

	// 默认相机模式：有目标用配置的跟随模式；无目标退化为自由视角（保证任何时候都有画面）
	CameraMode = (CurrentTargetIndex != INDEX_NONE) ? DefaultFollowCameraMode : EGameSpectatorCameraMode::FreeCam;

	ApplyToSpectatorPawn();

	// 把相机显式混入观察者 Pawn：
	// 死亡流程此前可能已经做过"复仇镜头"（把视图切到击杀者），这里再用一个短暂混入做转场，
	// 比引擎默认的硬切更符合竞技观战的观感。
	if (AGamePlayerController* GamePC = GetGamePlayerController())
	{
		if (AGameSpectatorPawn* SpectatorPawn = GetSpectatorPawn())
		{
			GamePC->SetViewTargetWithBlend(SpectatorPawn, ViewTargetBlendSeconds);
		}
		else
		{
			// 观察者 Pawn 缺失：最常见原因是 GameMode 的 SpectatorClass 未指向 AGameSpectatorPawn 子类，
			// 或客户端的 GameState->SpectatorClass 尚未复制到（进对局后一般已就绪）。
			UE_LOG(LogZombieKing, Warning,
				TEXT("[%s] 进入观战但未生成观察者 Pawn：观战相机不可用。"
				     "请检查 GameMode 的 SpectatorClass 是否为 AGameSpectatorPawn 的子类。"),
				*GetName());
		}
	}

	// 音频混音机械跟随：进入观战切 Spectate（先于事件广播 —— Lua 订阅者随后仍可覆盖）
	ApplySpectatorAudioState(true);

	OnSpectatorStateChanged.Broadcast(true);
	OnCameraModeChanged.Broadcast(CameraMode);
	OnTargetChanged.Broadcast(GetTargetPawnAt(CurrentTargetIndex));
}

void UGameSpectatorComponent::StopSpectating()
{
	if (!bSpectating)
	{
		return;
	}
	bSpectating = false;

	// 清空目标缓存与索引：避免下次进入观战时引用到上一局的失效目标
	SpectatableTargets.Reset();
	CurrentTargetIndex = INDEX_NONE;

	// 通知观察者 Pawn 解除跟随（它随后会由控制器在重新附身时被销毁/接管）
	if (AGameSpectatorPawn* SpectatorPawn = GetSpectatorPawn())
	{
		SpectatorPawn->SetFollowTarget(nullptr);
	}

	// 音频混音机械跟随：退回对局混音。
	// 为什么恢复为 Match 而不是"交给别人"：观战只发生在对局内，退出观战的常态就是
	// "重新附身回战场"；而复活不触发前端流程阶段变化（FlowStage 仍是 Match），
	// 没有别人会替我们恢复 Match 混音。至于"离局/切图"场景：前端流程阶段变化
	// （HandleFlowStageChanged）随后会重设状态，短暂重叠被混音淡入淡出吸收，无实际影响。
	ApplySpectatorAudioState(false);

	OnSpectatorStateChanged.Broadcast(false);
	OnTargetChanged.Broadcast(nullptr);
}

// =============================================================================
//  观战操作
// =============================================================================
void UGameSpectatorComponent::NextTarget()
{
	StepTarget(1);
}

void UGameSpectatorComponent::PrevTarget()
{
	StepTarget(-1);
}

void UGameSpectatorComponent::SetTargetIndex(int32 NewIndex)
{
	if (!bSpectating)
	{
		return;
	}

	// 重建后再设：调用方给的索引是针对"最新名册"的，先刷新列表才能保证索引语义正确
	RebuildTargetList();
	SetCurrentTargetIndexInternal(NewIndex, /*bBroadcast=*/true);
}

void UGameSpectatorComponent::SetCameraMode(EGameSpectatorCameraMode NewMode)
{
	// 哨兵值与重复设置直接忽略（避免无意义的下发与事件广播）
	if (NewMode == EGameSpectatorCameraMode::MAX || CameraMode == NewMode)
	{
		return;
	}

	CameraMode = NewMode;
	ApplyToSpectatorPawn();
	OnCameraModeChanged.Broadcast(NewMode);
}

void UGameSpectatorComponent::CycleCameraMode()
{
	// 循环顺序：自由 → 跟随·第一人称 → 跟随·第三人称 → 自由
	EGameSpectatorCameraMode NewMode = EGameSpectatorCameraMode::FreeCam;
	switch (CameraMode)
	{
	case EGameSpectatorCameraMode::FreeCam:
		NewMode = EGameSpectatorCameraMode::FollowFirstPerson;
		break;
	case EGameSpectatorCameraMode::FollowFirstPerson:
		NewMode = EGameSpectatorCameraMode::FollowThirdPerson;
		break;
	case EGameSpectatorCameraMode::FollowThirdPerson:
	default:
		NewMode = EGameSpectatorCameraMode::FreeCam;
		break;
	}

	// 切到"跟随"但当前没有目标时自动选一个目标；
	// 否则跟随模式会因为无目标而退化为自由视角，玩家会以为"按键没生效"。
	if (NewMode != EGameSpectatorCameraMode::FreeCam && GetTargetPawnAt(CurrentTargetIndex) == nullptr)
	{
		RebuildTargetList();
		SetCurrentTargetIndexInternal(SpectatableTargets.Num() > 0 ? 0 : INDEX_NONE, /*bBroadcast=*/true);
	}

	SetCameraMode(NewMode);
}

// =============================================================================
//  巡检（TickComponent）：目标阵亡/离场后自动接管
// =============================================================================
void UGameSpectatorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 只在"本机客户端正处于观战"时巡检：服务器上的 PC 也挂着本组件，但不需要也不应该做任何事。
	if (!bSpectating)
	{
		return;
	}
	AGamePlayerController* GamePC = GetGamePlayerController();
	if (!GamePC || !GamePC->IsLocalController())
	{
		return;
	}

	// 记录"当前目标本体"，重建后按本体重新定位（索引会因为名单变化而漂移，本体才是稳定的身份）
	APawn* PreviousTarget = GetTargetPawnAt(CurrentTargetIndex);
	RebuildTargetList();

	if (SpectatableTargets.Num() == 0)
	{
		// 场上暂时没有可观察的人（都被感染/阵亡且尚未复活）：清掉目标，相机退化为自由视角
		if (CurrentTargetIndex != INDEX_NONE || PreviousTarget != nullptr)
		{
			SetCurrentTargetIndexInternal(INDEX_NONE, /*bBroadcast=*/true);
		}
		return;
	}

	// 当前目标是否仍在列表中
	int32 FoundIndex = INDEX_NONE;
	for (int32 TargetIdx = 0; TargetIdx < SpectatableTargets.Num(); ++TargetIdx)
	{
		if (SpectatableTargets[TargetIdx].Get() == PreviousTarget)
		{
			FoundIndex = TargetIdx;
			break;
		}
	}

	if (FoundIndex != INDEX_NONE)
	{
		// 目标本体仍在：只修正因名单变化导致的索引位移，且不广播（目标没变，HUD 无需刷新）
		if (FoundIndex != CurrentTargetIndex)
		{
			SetCurrentTargetIndexInternal(FoundIndex, /*bBroadcast=*/false);
		}
		return;
	}

	// 目标本体已消失（阵亡/离场）：
	//  - 若本就在跟随模式，或此前确实有目标（说明是被动失去的），则自动接管到下一名玩家，保持观战连续；
	//  - 若本就在自由视角且从未选过目标，则不自动选人（避免"自由视角却报正在观战某人"）。
	const bool bHadTarget = (PreviousTarget != nullptr);
	if (bHadTarget || CameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		// 接管到"原索引位置"（夹取到列表范围），比一律跳回第一个更连续（例如原来看第 3 名，他死了就看新的第 3 名）
		const int32 FallbackIndex = (CurrentTargetIndex == INDEX_NONE) ? 0 : CurrentTargetIndex;
		SetCurrentTargetIndexInternal(FallbackIndex, /*bBroadcast=*/true);
	}
}

// =============================================================================
//  内部实现
// =============================================================================
void UGameSpectatorComponent::RebuildTargetList()
{
	SpectatableTargets.Reset();

	AGamePlayerController* GamePC = GetGamePlayerController();
	UWorld* World = GamePC ? GamePC->GetWorld() : nullptr;
	AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	if (!GamePC || !GameState)
	{
		return;
	}

	// 以 GameState 的玩家名册为准（它是对局玩家集合的唯一权威，服务器与客户端都有维护）
	APlayerState* OwningPlayerState = GamePC->PlayerState;
	for (const TObjectPtr<APlayerState>& PlayerState : GameState->PlayerArray)
	{
		// 排除自己：观战者本人不应出现在自己的可观察列表里
		if (!PlayerState || PlayerState.Get() == OwningPlayerState)
		{
			continue;
		}

		// 取该玩家的控制器与其当前 Pawn：没有 Pawn（阵亡/转观察者/尚未复活）的玩家自动被排除，
		// 因此列表天然只含"场上有可观看身体"的玩家。
		APlayerController* OtherController = PlayerState->GetPlayerController();
		if (!OtherController)
		{
			continue;
		}
		APawn* OtherPawn = OtherController->GetPawn();
		if (!IsValid(OtherPawn))
		{
			continue;
		}

		SpectatableTargets.Add(OtherPawn);
	}
}

void UGameSpectatorComponent::ApplyToSpectatorPawn()
{
	AGameSpectatorPawn* SpectatorPawn = GetSpectatorPawn();
	if (!SpectatorPawn)
	{
		// 服务器与"已退出观战"的本机都没有观察者 Pawn：静默返回是正确行为
		return;
	}

	SpectatorPawn->SetCameraMode(CameraMode);
	SpectatorPawn->SetFollowTarget(GetTargetPawnAt(CurrentTargetIndex));
}

void UGameSpectatorComponent::SetCurrentTargetIndexInternal(int32 NewIndex, bool bBroadcast)
{
	const int32 TargetCount = SpectatableTargets.Num();

	// 夹取到合法范围：TargetCount 为 0 或无目标（INDEX_NONE）时统一记为 INDEX_NONE
	int32 ClampedIndex = INDEX_NONE;
	if (TargetCount > 0 && NewIndex != INDEX_NONE)
	{
		ClampedIndex = FMath::Clamp(NewIndex, 0, TargetCount - 1);
	}

	APawn* PreviousTarget = GetTargetPawnAt(CurrentTargetIndex);
	CurrentTargetIndex = ClampedIndex;

	// 下发给观察者 Pawn（相机模式 + 目标）
	ApplyToSpectatorPawn();

	// 只在"目标本体确实变了"时广播：索引位移不构成一次真正的切换（避免 HUD 无谓刷新）
	APawn* NewTarget = GetTargetPawnAt(CurrentTargetIndex);
	if (bBroadcast && NewTarget != PreviousTarget)
	{
		OnTargetChanged.Broadcast(NewTarget);
	}
}

APawn* UGameSpectatorComponent::GetTargetPawnAt(int32 Index) const
{
	if (!SpectatableTargets.IsValidIndex(Index))
	{
		return nullptr;
	}
	return SpectatableTargets[Index].Get();
}

// =============================================================================
//  音频混音机械跟随
// =============================================================================
void UGameSpectatorComponent::ApplySpectatorAudioState(bool bEnterSpectating)
{
	// 内容侧开关关闭（特殊玩法要求观战保留战斗混音等）：完全交由调用方/Lua 接管
	if (!bSwitchAudioStateOnSpectate)
	{
		return;
	}

	// 经 Owner（玩家控制器）取 GameInstance 层音频服务。
	// 说明：C++ 内部可以直接用模板版 GetSubsystem（Lua 才需要绕 USubsystemBlueprintLibrary）。
	const AGamePlayerController* GamePC = GetGamePlayerController();
	UGameAudioSubsystem* AudioSubsystem =
		GamePC ? GamePC->GetGameInstance()->GetSubsystem<UGameAudioSubsystem>() : nullptr;
	if (!AudioSubsystem)
	{
		// GameInstance/服务尚未就绪（极端时序）：静默降级，观战照常、只是不切混音
		return;
	}

	// 幂等：音频服务内部对"状态未变"与重复 Push/Pop 已做保护。
	// 命名注意：参数刻意叫 bEnterSpectating 而非 bSpectating —— 后者是本类成员，重名触发 C4458。
	AudioSubsystem->SetAudioState(bEnterSpectating ? EGameAudioState::Spectate : EGameAudioState::Match);
}

void UGameSpectatorComponent::StepTarget(int32 Step)
{
	if (!bSpectating || Step == 0)
	{
		return;
	}

	// 先记录当前目标本体：重建列表后要按本体（而非索引）重新定位
	APawn* PreviousTarget = GetTargetPawnAt(CurrentTargetIndex);
	RebuildTargetList();

	const int32 TargetCount = SpectatableTargets.Num();
	if (TargetCount == 0)
	{
		SetCurrentTargetIndexInternal(INDEX_NONE, /*bBroadcast=*/true);
		return;
	}

	// 在（重建后的）列表里定位当前目标；找不到说明它已阵亡/离场，则从列表头/尾重新开始
	int32 PreviousIndex = INDEX_NONE;
	if (PreviousTarget)
	{
		for (int32 TargetIdx = 0; TargetIdx < TargetCount; ++TargetIdx)
		{
			if (SpectatableTargets[TargetIdx].Get() == PreviousTarget)
			{
				PreviousIndex = TargetIdx;
				break;
			}
		}
	}

	int32 NewIndex = INDEX_NONE;
	if (PreviousIndex == INDEX_NONE)
	{
		NewIndex = (Step > 0) ? 0 : (TargetCount - 1);
	}
	else
	{
		// 环形步进：(a % n + n) % n 保证负数取模在 C++ 下也落在 [0, n)
		NewIndex = ((PreviousIndex + Step) % TargetCount + TargetCount) % TargetCount;
	}

	// 从自由视角切目标 = 玩家明确表达了"我要看这个人"，因此自动切到配置的默认跟随模式
	if (CameraMode == EGameSpectatorCameraMode::FreeCam && DefaultFollowCameraMode != EGameSpectatorCameraMode::FreeCam)
	{
		SetCameraMode(DefaultFollowCameraMode);
	}

	SetCurrentTargetIndexInternal(NewIndex, /*bBroadcast=*/true);
}

// =============================================================================
//  访问器
// =============================================================================
AGamePlayerController* UGameSpectatorComponent::GetGamePlayerController() const
{
	// 本组件固定挂在 AGamePlayerController 上（由 AGamePlayerController 构造期创建）
	return Cast<AGamePlayerController>(GetOwner());
}

AGameSpectatorPawn* UGameSpectatorComponent::GetSpectatorPawn() const
{
	if (const AGamePlayerController* GamePC = GetGamePlayerController())
	{
		// 由引擎的观察者状态机生成并持有（见 APlayerController::SpawnSpectatorPawn）
		return Cast<AGameSpectatorPawn>(GamePC->GetSpectatorPawn());
	}
	return nullptr;
}

// =============================================================================
//  查询接口实现
// =============================================================================
AActor* UGameSpectatorComponent::GetCurrentTarget() const
{
	return GetTargetPawnAt(CurrentTargetIndex);
}

TArray<AActor*> UGameSpectatorComponent::GetSpectatableTargets() const
{
	TArray<AActor*> Result;
	Result.Reserve(SpectatableTargets.Num());
	for (const TWeakObjectPtr<APawn>& WeakPawn : SpectatableTargets)
	{
		if (APawn* TargetPawn = WeakPawn.Get())
		{
			Result.Add(TargetPawn);
		}
	}
	return Result;
}
