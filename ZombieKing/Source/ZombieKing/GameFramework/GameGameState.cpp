#include "GameGameState.h"
#include "Net/UnrealNetwork.h"    // DOREPLIFETIME 所依赖的复制注册宏
#include "Engine/World.h"         // GetWorld() / GetTimerManager（阶段倒计时定时器）
#include "TimerManager.h"         // FTimerManager（服务器权威倒计时）
#include "GameFramework/Controller.h"            // AController（ReportKill 的形参）
#include "GameFramework/PlayerState.h"           // APlayerState::GetPlayerName（击杀播报取玩家名）
#include "GenericTeamAgentInterface.h"           // IGenericTeamAgentInterface（取玩家阵营）

AGameGameState::AGameGameState()
{
	// 显式开启复制：
	// GameState 承载"所有人可见"的对局信息（阶段/时间/比分），必须保证全网同步；
	// 显式置位可防止蓝图子类误改为不可复制，导致回合判定与 UI 各自为政。
	bReplicates = true;

	// 阶段默认从"大厅"起步，等待 Lua/GameMode 规则推进到对局阶段。
	MatchPhase = EGameMatchPhase::Lobby;
}

void AGameGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	// 先让基类（AGameStateBase）注册其自身需要复制的成员，避免漏注册
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// 四个字段均为公开对局信息，使用 COND_None 对全部连接复制：
	//   - MatchPhase               ：当前阶段，供 HUD/回合横幅使用；
	//   - MatchPhaseEndServerTime  ：阶段结束时间戳，客户端据此本地倒计时；
	//   - TeamScores               ：阵营比分，供比分板使用；
	//   - KillFeed                 ：击杀播报列表，供 HUD 播报区使用。
	DOREPLIFETIME(AGameGameState, MatchPhase);
	DOREPLIFETIME(AGameGameState, MatchPhaseEndServerTime);
	DOREPLIFETIME(AGameGameState, TeamScores);
	DOREPLIFETIME(AGameGameState, KillFeed);
}

float AGameGameState::GetMatchPhaseRemainingTime() const
{
	// 无有效结束时间（例如大厅无限等待）时返回 0，语义为"不适用倒计时"。
	if (MatchPhaseEndServerTime <= 0.0f)
	{
		return 0.0f;
	}

	// 使用 GetServerWorldTimeSeconds()：该时间已被引擎做过客户端时延补偿，
	// 客户端据此本地倒计时，无需服务器逐帧复制"剩余时间"，从而省带宽并避免网络抖动。
	return FMath::Max(0.0f, MatchPhaseEndServerTime - GetServerWorldTimeSeconds());
}

int32 AGameGameState::GetTeamScore(uint8 InTeamId) const
{
	// 线性查找阵营条目：阵营数量极少（初期 2 个），线性查找足够且无需额外维护映射结构。
	for (const FGameTeamScore& Entry : TeamScores)
	{
		if (Entry.TeamId == InTeamId)
		{
			return Entry.Score;
		}
	}

	// 该阵营尚无记分条目时按 0 处理，避免调用方到处判空。
	return 0;
}

void AGameGameState::SetMatchPhase(EGameMatchPhase NewPhase)
{
	// 服务器权威校验：只有服务器能改写对局阶段，客户端调用直接忽略（防作弊）
	if (!HasAuthority())
	{
		return;
	}

	// 阶段未变化则短路：避免无意义的复制与事件广播，减少带宽与表现层重复触发
	if (MatchPhase == NewPhase)
	{
		return;
	}

	// 先保存旧阶段，供广播时携带"新旧对比"信息
	const EGameMatchPhase OldPhase = MatchPhase;

	// 写入新阶段：客户端将通过网络复制在 OnRep_MatchPhase 中感知
	MatchPhase = NewPhase;

	// 服务器本地不会触发 OnRep（OnRep 仅在客户端生效），
	// 因此服务器必须手动广播一次，保证本地监听者（GameMode / HUD / Lua 脚本）与远端同步收到事件。
	OnGameMatchPhaseChanged.Broadcast(MatchPhase, OldPhase);
}

void AGameGameState::SetMatchPhaseEndServerTime(float NewEndServerTime)
{
	// 仅服务器可写：结束时间戳是对局节奏的权威数据，客户端不得篡改
	if (!HasAuthority())
	{
		return;
	}

	// 直接写入；客户端通过复制获得该时间戳后自行计算剩余时间（无需额外广播）
	MatchPhaseEndServerTime = NewEndServerTime;

	// -------------------------------------------------------------------------
	// 重新武装阶段倒计时定时器（"到点通知"机制，服务器独占）
	// -------------------------------------------------------------------------
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	FTimerManager& TimerManager = World->GetTimerManager();

	// 每次设置结束时间都代表"当前阶段被推进或被重置"，先清除旧定时器，
	// 否则上一阶段的倒计时会在新阶段中途错误触发（例如开局瞬间又收到一次"对局结束"）。
	TimerManager.ClearTimer(MatchPhaseTimerHandle);

	if (NewEndServerTime > 0.0f)
	{
		// 剩余时长以"服务器世界时间"为准；钳一个极小正值，避免结束时间已过时立即同步回调造成重入
		const float Remaining = FMath::Max(NewEndServerTime - GetServerWorldTimeSeconds(), KINDA_SMALL_NUMBER);
		// bLoop=false：一次性定时器（每次进入阶段都会重新设置，无需循环）
		TimerManager.SetTimer(MatchPhaseTimerHandle, this, &AGameGameState::HandleMatchPhaseTimerElapsed, Remaining, false);
	}
}

void AGameGameState::HandleMatchPhaseTimerElapsed()
{
	// 双保险：定时器只在服务器武装，但回调仍做一次权威校验，防止异常路径下的客户端执行
	if (!HasAuthority())
	{
		return;
	}

	// 记录"到点的是哪个阶段"：Lobby 与 Match 都可能带倒计时，订阅方需要据此区分处理
	const EGameMatchPhase ExpiredPhase = MatchPhase;

	// 倒计时归零：清空结束时间戳（客户端剩余时间随之为 0，倒计时 UI 停止刷新）。
	// 注意此处直接写成员而非走 SetMatchPhaseEndServerTime()，避免"清零再重新武装"的多余定时器操作。
	MatchPhaseEndServerTime = 0.0f;

	// 服务器不会因赋值触发 OnRep，必须显式广播事件，让 Lua 规则脚本执行结算。
	// 注意：Lua 处理器内可能再次推进阶段并重设结束时间，从而重新武装定时器，形成"回合循环"。
	OnGameMatchPhaseExpired.Broadcast(ExpiredPhase);
}

void AGameGameState::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 清除尚未触发的阶段定时器：对局销毁/关卡切换后若定时器仍存活，会回调到已失效的对象上
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(MatchPhaseTimerHandle);
	}

	Super::EndPlay(EndPlayReason);
}

void AGameGameState::SetTeamScore(uint8 InTeamId, int32 NewScore)
{
	// 仅服务器可写：比分是对局胜负的权威依据，严禁客户端参与
	if (!HasAuthority())
	{
		return;
	}

	// 先尝试更新已有阵营条目
	for (FGameTeamScore& Entry : TeamScores)
	{
		if (Entry.TeamId == InTeamId)
		{
			Entry.Score = NewScore;
			// 服务器不会因赋值触发 OnRep，需手动广播，驱动本地 HUD 比分板刷新
			OnGameTeamScoresChanged.Broadcast();
			return;
		}
	}

	// 未找到则新增一个阵营记分条目（服务器是唯一写入方，无需考虑并发冲突）
	FGameTeamScore NewEntry;
	NewEntry.TeamId = InTeamId;
	NewEntry.Score = NewScore;
	TeamScores.Add(NewEntry);

	// 新增条目同样广播一次，保证首条比分写入时 HUD 也能立即刷新
	OnGameTeamScoresChanged.Broadcast();
}

void AGameGameState::AddTeamScore(uint8 InTeamId, int32 DeltaScore)
{
	// 仅服务器可写
	if (!HasAuthority())
	{
		return;
	}

	// 复用 SetTeamScore 完成"读取现值 + 累加增量"的原子写入，避免两处重复维护查找逻辑
	SetTeamScore(InTeamId, GetTeamScore(InTeamId) + DeltaScore);
}

void AGameGameState::OnRep_MatchPhase(EGameMatchPhase OldPhase)
{
	// 客户端收到阶段变化：广播事件，驱动 HUD 横幅、回合提示与 Lua 表现逻辑刷新
	OnGameMatchPhaseChanged.Broadcast(MatchPhase, OldPhase);
}

void AGameGameState::OnRep_MatchPhaseEndServerTime()
{
	// 结束时间发生变化：客户端可据此重启倒计时显示。
	// 当前无需额外逻辑，保留钩子以便后续接入 UI 倒计时重启或阶段提示音。
}

void AGameGameState::OnRep_TeamScores()
{
	// 比分发生变化：客户端广播事件，驱动比分板动画/领先方提示（表现由 Lua 订阅处理）。
	OnGameTeamScoresChanged.Broadcast();
}

// =============================================================================
//  击杀播报（KillFeed）
// =============================================================================

void AGameGameState::ReportKill(AController* KillerController, AController* VictimController, bool bHeadshot)
{
	// 服务器权威：击杀事实只由服务器认定（客户端调用被静默忽略）
	if (!HasAuthority())
	{
		return;
	}

	// 受害者必须有控制器（没有受害者的"击杀"不成立，属调用方错误）
	if (!VictimController)
	{
		return;
	}

	// -------------------------------------------------------------------------
	// 组装条目：名字与阵营一律在这里从 PlayerState 取（Lua 侧不需要自己拼字符串）
	// -------------------------------------------------------------------------
	FGameKillFeedEntry Entry;
	Entry.bHeadshot = bHeadshot;

	if (const APlayerState* VictimState = VictimController->PlayerState)
	{
		Entry.VictimName = VictimState->GetPlayerName();
		// 阵营经通用接口取（PlayerState 实现了 IGenericTeamAgentInterface）；取不到则保持 NoTeam(255)
		if (const IGenericTeamAgentInterface* VictimTeam = Cast<IGenericTeamAgentInterface>(VictimState))
		{
			Entry.VictimTeamId = VictimTeam->GetGenericTeamId().GetId();
		}
	}

	// 击杀者可能为空（环境伤害、摔落）或与受害者相同（自杀）：此时名字留空，HUD 侧自行显示"环境击杀/自杀"
	if (KillerController && KillerController != VictimController)
	{
		if (const APlayerState* KillerState = KillerController->PlayerState)
		{
			Entry.KillerName = KillerState->GetPlayerName();
			if (const IGenericTeamAgentInterface* KillerTeam = Cast<IGenericTeamAgentInterface>(KillerState))
			{
				Entry.KillerTeamId = KillerTeam->GetGenericTeamId().GetId();
			}
		}
	}

	// -------------------------------------------------------------------------
	// 追加并裁剪到上限（保留最新的 MaxKillFeedEntries 条，丢弃最旧的）
	// -------------------------------------------------------------------------
	KillFeed.Add(MoveTemp(Entry));
	const int32 OverflowCount = KillFeed.Num() - FMath::Max(MaxKillFeedEntries, 1);
	if (OverflowCount > 0)
	{
		KillFeed.RemoveAt(0, OverflowCount);
	}

	// 服务器不会因赋值触发 OnRep：手动广播，保证本端（ListenServer 的 HUD）与远端同步刷新
	OnKillFeedChanged.Broadcast();
}

void AGameGameState::ClearKillFeed()
{
	// 仅服务器可清（回合重置时调用）；避免上一局的播报残留在新回合
	if (!HasAuthority())
	{
		return;
	}

	if (KillFeed.Num() == 0)
	{
		return;	// 本就为空则不做无谓广播
	}

	KillFeed.Reset();
	OnKillFeedChanged.Broadcast();
}

void AGameGameState::OnRep_KillFeed()
{
	// 客户端收到播报变化：广播事件，HUD（Lua）订阅后调 GetKillFeed 刷新播报区
	OnKillFeedChanged.Broadcast();
}
