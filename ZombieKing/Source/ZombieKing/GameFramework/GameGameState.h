#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "GameGameState.generated.h"     // UHT 生成头文件，必须置于最后

class AController;   // ReportKill 的形参类型（控制器引用；实现里再从它的 PlayerState 取名字与阵营）

/**
 * 对局阶段枚举（生化模式六阶段回合循环）
 *
 * 设计契约（务必遵守）：
 *  - 本枚举是"复制契约"的一部分（阶段值随网络复制），增删/改序会改变协议取值 —— 只有这类
 *    "改数据结构/复制协议"才允许动 C++；
 *  - "何时进入哪个阶段、每个阶段多久、到点后做什么"全部是**规则**，写在 Lua 规则脚本
 *    （Content/Script/Lua/Core/GameFramework/GameMode.lua），改规则不需要重编译 C++；
 *  - C++ 侧（本类）只提供通用机制：阶段复制 + 阶段结束时间戳 + 权威倒计时 + 变化/到期广播，
 *    对任何阶段取值一视同仁，不含玩法判断。
 *
 * 回合循环（规则口径，供对齐）：
 *   Lobby(等人) → Warmup(热身) → Infection(母体选拔+变身选择窗口)
 *   → Combat(人类抵抗主战斗) → Settlement(结算展示) → Intermission(休整) → 回到 Warmup
 *
 * 玩法门控约定（由 Lua 判定，C++ 不硬编码）：
 *  - "命中即感染"与"伤害/战绩统计"仅在 Infection 与 Combat 两个阶段开放；
 *  - Warmup 全员人类、无感染压力，仅作出生与走位准备；
 *  - Settlement/Intermission 期间不做有效伤害判定（死亡按兜底转观察者处理）。
 */
UENUM(BlueprintType)
enum class EGameMatchPhase : uint8
{
	/** 大厅/等待：凑人阶段，等待玩家入场达到开局人数（通常无倒计时，人满即推进） */
	Lobby UMETA(DisplayName = "Lobby (大厅等待)"),

	/** 热身：全员人类，开放移动与出生，不产生母体、不做感染判定 */
	Warmup UMETA(DisplayName = "Warmup (热身准备)"),

	/** 感染爆发：按比例选拔母体并进入"变身选择"窗口；命中即感染自本阶段开放 */
	Infection UMETA(DisplayName = "Infection (感染爆发)"),

	/** 人类抵抗：对局主战斗阶段，倒计时在此阶段决定"人类撑到时间结束即获胜" */
	Combat UMETA(DisplayName = "Combat (人类抵抗)"),

	/** 回合结算：胜负已定、比分已写入，展示结算信息（玩家暂不重置，供观战/停留） */
	Settlement UMETA(DisplayName = "Settlement (回合结算)"),

	/** 局间休整：玩家已重置回人类，等待下一回合开局（人数不足则回 Lobby） */
	Intermission UMETA(DisplayName = "Intermission (局间休整)"),

	/** 枚举哨兵：用于数组容量与越界校验，绝不作为真实阶段使用 */
	MAX UMETA(Hidden)
};

/**
 * 阵营记分条目（复制用数据结构）
 *
 * 说明：以"阵营 ID + 得分"的条目数组对外暴露，而非写死两个 int32，
 *       这样未来从"人类 vs 生化"扩展到多阵营时无需改动复制协议（只改数据内容）。
 */
USTRUCT(BlueprintType)
struct FGameTeamScore
{
	GENERATED_BODY()

	/** 阵营 ID（对应 FGenericTeamId；人类与生化幽灵分属不同阵营） */
	UPROPERTY(BlueprintReadOnly, Category = "Score")
	uint8 TeamId = 0;

	/** 该阵营累计得分（回合胜利数等；具体计分规则由 Lua/GameMode 决定） */
	UPROPERTY(BlueprintReadOnly, Category = "Score")
	int32 Score = 0;
};

/**
 * 击杀播报条目（复制用数据结构）
 *
 * 设计说明：只放"显示所需的最终数据"（名字/阵营/是否爆头），不做任何字符串拼接的本地化处理 ——
 *  - 玩家名由引擎的 PlayerState 提供，本身不该翻译；
 *  - 阵营/爆头用结构化字段携带，HUD（Lua/WBP）按需渲染（例如爆头加图标、按阵营着色）。
 * 这样"播报长什么样"完全留给表现层，C++ 只负责"事实与复制"。
 */
USTRUCT(BlueprintType)
struct FGameKillFeedEntry
{
	GENERATED_BODY()

	/** 击杀者名字（环境/自杀伤害时为空串） */
	UPROPERTY(BlueprintReadOnly, Category = "KillFeed")
	FString KillerName;

	/** 受害者名字 */
	UPROPERTY(BlueprintReadOnly, Category = "KillFeed")
	FString VictimName;

	/** 击杀者阵营 ID（对应 FGenericTeamId；无归属时为 255 = NoTeam） */
	UPROPERTY(BlueprintReadOnly, Category = "KillFeed")
	uint8 KillerTeamId = 255;

	/** 受害者阵营 ID */
	UPROPERTY(BlueprintReadOnly, Category = "KillFeed")
	uint8 VictimTeamId = 255;

	/** 是否为爆头击杀（HUD 据此加爆头标记） */
	UPROPERTY(BlueprintReadOnly, Category = "KillFeed")
	bool bHeadshot = false;
};

/**
 * 击杀播报变化广播（服务器写入后本地广播、客户端 OnRep 时广播）
 * 说明：事件不带参数，订阅方（Lua HUD）收到后自行调 GetKillFeed() 取最新列表刷新，
 *       避免每次击杀都复制一份完整列表到事件里。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGameKillFeedChanged);

/**
 * 对局阶段变化广播（动态多播委托）
 *
 * 选择"动态"（Dynamic）多播委托的原因：
 *  - 可由蓝图与 UnLua 脚本直接绑定（UnLua 支持绑定 BlueprintAssignable 委托），
 *    从而让"阶段变化后的表现逻辑"留在可热更的 Lua 侧；
 *  - C++ 侧亦可通过 UFUNCTION 回调订阅。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameMatchPhaseChanged, EGameMatchPhase, NewPhase, EGameMatchPhase, OldPhase);

/**
 * 阶段倒计时到期广播（服务器权威，携带"刚刚到期的阶段"）
 *
 * 为什么需要它：对局胜负判定是"规则"（必须可热更，写在 Lua），
 * 但"时间到了"这个事实属于不可热更的机制，必须由 C++ 权威计时器给出。
 * 于是分工为：C++ 负责在服务器精确到点时广播本事件；Lua 规则脚本订阅它，
 * 再自行决定"是对局结束（人类胜）还是间歇结束（开下一局）"。
 *
 * 之所以携带 ExpiredPhase：六阶段中除 Lobby（无限等待）外都可能有倒计时，
 * 订阅方（Lua 规则）需要知道"到点的是哪一个阶段"才能做出正确推进，避免来回查询竞态。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameMatchPhaseExpired, EGameMatchPhase, ExpiredPhase);

/**
 * 阵营比分变化广播（服务器与客户端都会广播，无参数）
 *
 * 为什么需要它：比分由 GameState 复制，但"复制"只负责同步数值本身，不会通知订阅方；
 * HUD（比分板）需要"数值一变就刷新"的响应式信号，故提供本事件：
 *  - 服务器：在 AddTeamScore/SetTeamScore 写入后本地广播（服务器自身没有 OnRep）；
 *  - 客户端：在 OnRep_TeamScores 中广播。
 * 订阅方（Lua HUD 脚本）收到后自行调用 GetTeamScore 取值刷新（事件不带参数，避免复制多余数据）。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGameTeamScoresChanged);

/**
 * 竞技生化网游：对局全局状态（GameState 契约骨架）
 *
 * 架构定位（工业级竞技网游规范）：
 *  - GameState 是"对所有客户端可见的权威对局信息"的唯一复制载体：阶段、阶段时间、阵营比分；
 *  - 写入权归服务器（GameMode / Lua 规则脚本），客户端只读并监听变化；
 *  - 与 AGamePlayerState（承载单玩家数据 + GAS）职责分离：GameState 管"局"，PlayerState 管"人"。
 *
 * 职责边界与热更新原则：
 *  - 本类（C++）只提供"不可热更的契约"——复制字段、权威写入接口、查询接口、变化委托；
 *  - "何时切换阶段""如何判定胜负""如何计算比分"等规则会随运营频繁调整，
 *    一律不得硬编码在此，必须由 UnLua 脚本通过下方 BlueprintCallable 接口驱动与判定。
 *    判断标准：改规则要不要重编译 C++？要 → 该规则必须写在 Lua。
 */
UCLASS()
class ZOMBIEKING_API AGameGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	/**
	 * 构造函数：仅做最基础的默认值设定，不参与任何玩法规则。
	 */
	AGameGameState();

	// -------------------------------------------------------------------------
	//  网络复制
	// -------------------------------------------------------------------------
	/**
	 * 注册需要复制的对局状态字段。
	 * 阶段/阶段时间/阵营比分都是"人人可见的公开对局信息"，故统一使用 DOREPLIFETIME（COND_None）。
	 */
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// -------------------------------------------------------------------------
	//  阶段变化广播（供 C++ / 蓝图 / UnLua 订阅）
	// -------------------------------------------------------------------------
	/**
	 * 对局阶段变化事件。
	 * 服务器在 SetMatchPhase 后本地广播、客户端在 OnRep_MatchPhase 中广播；
	 * Lua 侧可绑定此委托，把"阶段切换后的横幅/音效/UI/规则初始化"等可热更表现逻辑接到这里。
	 */
	UPROPERTY(BlueprintAssignable, Category = "GameState|Phase")
	FOnGameMatchPhaseChanged OnGameMatchPhaseChanged;

	/**
	 * 阶段倒计时到期事件（服务器权威）。
	 * 由本类的服务器定时器在 MatchPhaseEndServerTime 到达时广播；
	 * Lua 规则脚本订阅后执行"人类胜/生化胜/进入下一局"等结算（规则可热更）。
	 */
	UPROPERTY(BlueprintAssignable, Category = "GameState|Phase")
	FOnGameMatchPhaseExpired OnGameMatchPhaseExpired;

	/**
	 * 阵营比分变化事件。
	 * 服务器在本类写入比分后广播、客户端在 OnRep_TeamScores 中广播；
	 * HUD（比分板）订阅后自行读取 GetTeamScore 刷新显示。
	 */
	UPROPERTY(BlueprintAssignable, Category = "GameState|Score")
	FOnGameTeamScoresChanged OnGameTeamScoresChanged;

	// -------------------------------------------------------------------------
	//  只读查询接口（BlueprintPure：C++ / 蓝图 / Lua 均可调用）
	// -------------------------------------------------------------------------

	/** 获取当前对局阶段 */
	UFUNCTION(BlueprintPure, Category = "GameState|Phase")
	EGameMatchPhase GetMatchPhase() const { return MatchPhase; }

	/**
	 * 获取当前阶段的剩余时间（秒）。
	 * 客户端本地用服务器同步时间倒计时，避免服务器逐帧复制剩余时间（省带宽、防抖动）。
	 */
	UFUNCTION(BlueprintPure, Category = "GameState|Phase")
	float GetMatchPhaseRemainingTime() const;

	/** 获取指定阵营的累计得分；若该阵营尚无记分条目则返回 0 */
	UFUNCTION(BlueprintPure, Category = "GameState|Score")
	int32 GetTeamScore(uint8 InTeamId) const;

	/** 取击杀播报列表（最新的一条在末尾） */
	UFUNCTION(BlueprintPure, Category = "GameState|KillFeed")
	TArray<FGameKillFeedEntry> GetKillFeed() const { return KillFeed; }

	// -------------------------------------------------------------------------
	//  服务器权威写入接口
	//  说明：全部标记为 BlueprintCallable，以便 UnLua 规则脚本（可热更新）驱动；
	//        内部再做 HasAuthority 校验，客户端即便调用也会被安全忽略。
	// -------------------------------------------------------------------------

	/**
	 * 设置对局阶段（仅服务器生效）。
	 * 规则上"何时进入对局/何时回到大厅"由 Lua 脚本决定并调用本接口，本类不做规则判断。
	 */
	UFUNCTION(BlueprintCallable, Category = "GameState|Phase")
	void SetMatchPhase(EGameMatchPhase NewPhase);

	/**
	 * 设置当前阶段的服务器结束时间戳（仅服务器生效）。
	 * 传 0 表示"该阶段无倒计时"（例如大厅无限等待）；客户端据此本地计算剩余时间。
	 */
	UFUNCTION(BlueprintCallable, Category = "GameState|Phase")
	void SetMatchPhaseEndServerTime(float NewEndServerTime);

	/** 直接设置某阵营的累计得分（仅服务器生效） */
	UFUNCTION(BlueprintCallable, Category = "GameState|Score")
	void SetTeamScore(uint8 InTeamId, int32 NewScore);

	/** 在某阵营累计得分上追加增量（仅服务器生效，供击杀/回合结算调用） */
	UFUNCTION(BlueprintCallable, Category = "GameState|Score")
	void AddTeamScore(uint8 InTeamId, int32 DeltaScore);

	// -------------------------------------------------------------------------
	//  击杀播报（KillFeed）：服务器权威写入，客户端只读并监听变化
	// -------------------------------------------------------------------------
	/**
	 * 记录一条击杀播报（仅服务器生效）。
	 *
	 * 分工：**"什么时候记一条击杀"是规则**（写在 Lua 规则脚本里，例如感染/击杀结算时调用本接口），
	 *       而"怎么组装数据、怎么复制、怎么裁剪长度"是机制（在本类里）—— 因此本接口接收控制器引用，
	 *       名字与阵营一律由 C++ 从 PlayerState 取，Lua 不需要自己拼字符串。
	 *
	 * @param KillerController 击杀者的控制器（环境死亡/自杀时为空或与受害者相同）
	 * @param VictimController 受害者的控制器（必须有）
	 * @param bHeadshot        是否爆头击杀（由规则侧判定并传入）
	 */
	UFUNCTION(BlueprintCallable, Category = "GameState|KillFeed")
	void ReportKill(AController* KillerController, AController* VictimController, bool bHeadshot);

	/** 清空击杀播报（仅服务器生效；通常在新回合开局时调用，避免上一局信息残留） */
	UFUNCTION(BlueprintCallable, Category = "GameState|KillFeed")
	void ClearKillFeed();

	/** 击杀播报变化事件（HUD 订阅后调 GetKillFeed 刷新） */
	UPROPERTY(BlueprintAssignable, Category = "GameState|KillFeed")
	FOnGameKillFeedChanged OnKillFeedChanged;

protected:
	// -------------------------------------------------------------------------
	//  复制数据成员（private 语义，仅允许通过上方权威接口在服务器改写）
	// -------------------------------------------------------------------------

	/** 当前对局阶段：服务器权威字段，变化时在客户端触发 OnRep_MatchPhase */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MatchPhase, Category = "GameState|Phase")
	EGameMatchPhase MatchPhase = EGameMatchPhase::Lobby;

	/** 当前阶段的服务器结束时间戳（秒）；<=0 表示该阶段无倒计时 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MatchPhaseEndServerTime, Category = "GameState|Phase")
	float MatchPhaseEndServerTime = 0.0f;

	/** 阵营比分列表：以条目数组承载，便于未来扩展阵营数量而不改复制协议 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_TeamScores, Category = "GameState|Score")
	TArray<FGameTeamScore> TeamScores;

	/**
	 * 击杀播报列表（最新的一条在末尾）。
	 * 用"复制数组"而不是"多播 RPC"承载：击杀是低频事件，数组整体复制的开销很小，
	 * 且天然解决了"中途加入的客户端看不到历史播报"的问题（数组会把当前列表整体同步给他）。
	 */
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_KillFeed, Category = "GameState|KillFeed")
	TArray<FGameKillFeedEntry> KillFeed;

	/** 击杀播报最多保留多少条（超出时丢弃最旧的；HUD 通常只显示最近几条） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "GameState|KillFeed", meta = (ClampMin = "1", ClampMax = "64"))
	int32 MaxKillFeedEntries = 8;

	// -------------------------------------------------------------------------
	//  复制回调（OnRep）：仅在客户端触发
	// -------------------------------------------------------------------------

	/** 阶段复制回调：广播 OnGameMatchPhaseChanged（携带新旧阶段） */
	UFUNCTION()
	virtual void OnRep_MatchPhase(EGameMatchPhase OldPhase);

	/** 阶段结束时间复制回调：客户端重启倒计时显示（保留钩子） */
	UFUNCTION()
	virtual void OnRep_MatchPhaseEndServerTime();

	/** 比分复制回调：客户端刷新比分板（保留钩子） */
	UFUNCTION()
	virtual void OnRep_TeamScores();

	/** 击杀播报复制回调：客户端广播 OnKillFeedChanged（HUD 订阅后取 GetKillFeed 刷新） */
	UFUNCTION()
	virtual void OnRep_KillFeed();

	// -------------------------------------------------------------------------
	//  阶段倒计时机制（服务器权威）
	//  说明：这里只负责"到点通知"，不决定到点后做什么——那是 Lua 规则脚本的职责。
	// -------------------------------------------------------------------------

	/** 阶段结束定时器回调：到期时清空结束时间戳并广播 OnGameMatchPhaseExpired */
	void HandleMatchPhaseTimerElapsed();

	/** Actor 结束：清除定时器，防止关卡切换/对局销毁后回调悬空 */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 阶段倒计时定时器句柄（仅服务器使用；每次重设结束时间会先清除旧定时器再重新武装） */
	FTimerHandle MatchPhaseTimerHandle;
};
