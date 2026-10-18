#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "UnLuaInterface.h"                  // IUnLuaInterface：把本 GameMode 绑定到 Lua 规则脚本
#include "GameFaction.h"                     // EGameFaction（阵营 → 默认角色类映射的键）
#include "GameGameMode.generated.h"

class AGamePlayerState;
class AGameGameState;
class AGameCharacterBase;

/**
 * 竞技生化网游：对局框架 GameMode（服务器权威的"局"级规则宿主 + 角色生成入口）
 *
 * 与演示类的关系：
 *  - 项目自带的 AZombieKingGameMode 属于 Epic 第一人称模板的演示类，本架构不依赖、也不改动它；
 *  - 本类才是正式框架层 GameMode，正式对局应使用本类（或其蓝图子类）。
 *
 * 职责边界（严格遵循热更新原则）：
 *  - C++ 只做三件事：① 装配对局框架类；② 派发框架级生命周期事件给 Lua；③ 提供"框架级动作"
 *    （按阵营生成角色、感染重生）——这类动作是引擎机制，不属于可调规则；
 *  - 任何"会随运营频繁调整的规则"——谁被感染、何时感染、胜负判定、计分——一律写在 Lua 规则脚本里。
 */
UCLASS(abstract)
class ZOMBIEKING_API AGameGameMode : public AGameModeBase,
                                      public IUnLuaInterface
{
    GENERATED_BODY()

public:
    AGameGameMode();

    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 返回本 GameMode 绑定的 Lua 脚本模块路径（相对 Content/Script） */
    // 注意：UnLua 的接口声明为 const，签名必须完全一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // 按阵营生成角色（框架级动作，服务器权威）
    // -------------------------------------------------------------------------
    /**
     * 把某玩家重新生成为指定阵营的角色（服务器权威）。
     * 内部依次完成：
     *  1) PlayerState 分配阵营（并切换对应属性集）；
     *  2) 决定重生使用的 Pawn 类：显式传入优先，否则取 FactionPawnClasses 中该阵营的默认类；
     *  3) 卸下并销毁旧 Avatar（会触发技能清除）；
     *  4) 重新生成并附身新 Avatar（新 Avatar 附身时会按"该角色的定义"灌入初始属性并授予技能集）。
     *
     * @param Controller         目标玩家控制器
     * @param Faction            目标阵营
     * @param PawnClassOverride  可选的显式角色类（为空则用该阵营的默认角色类）
     * @return 是否成功生成新 Avatar
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Spawn")
    bool RespawnPlayerAsFaction(AController* Controller, EGameFaction Faction, TSubclassOf<AGameCharacterBase> PawnClassOverride);

    /**
     * 把某玩家感染为生化幽灵（服务器权威）。
     * 只提供"机制"、不做"规则判断"：由 Lua 规则脚本决定"谁、何时、变成哪种幽灵"，再调用本函数。
     * 等价于 RespawnPlayerAsFaction(Controller, Zombie, 指定僵尸类)。
     *
     * @param Controller       被感染的玩家控制器
     * @param ZombiePawnClass  要变成的生化幽灵角色类（如母体 / 多足爬墙幽灵）
     * @return 是否成功生成僵尸 Avatar
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Infection")
    bool InfectPlayer(AController* Controller, TSubclassOf<AGameCharacterBase> ZombiePawnClass);

    // -------------------------------------------------------------------------
    // 变身选择（CF 式：变成幽灵时弹出选择框，从玩家仓库中挑角色；规则在 Lua、机制在此与 PC）
    // -------------------------------------------------------------------------
    /**
     * 发起"变身选择"（服务器权威；Lua 规则的入口 —— 开局选拔母体、攻击命中感染都走它）。
     *
     * 内部动作（一次调用内原子完成前两步）：
     *  1) 阵营立即切换为生化幽灵（保持"感染即生效"的规则链：胜负判定/重复感染拦截立刻正确，
     *     无需等玩家选完角色）；
     *  2) 卸下并销毁其人类 Avatar（选择期间该玩家无 Avatar、不可操作，与感染语义一致；
     *     选择框为全屏模态，表现由内容侧 WBP 覆盖）；
     *  3) 交给 AGamePlayerController 的变身选择状态机：通知客户端弹框（倒计时）→
     *     玩家从仓库选 / 超时 → 服务器校验后经 InfectPlayer 生成所选幽灵 → 派发 OnGhostTransformed。
     *
     * @param Controller       要变身的玩家控制器（必须当前为人类阵营）
     * @param DurationSeconds  选择倒计时秒数（规则数值，由 Lua CONFIG 提供，可热更）
     * @return 是否成功进入变身选择（重复发起 / 非人类阵营 / 缺组件返回 false）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|GhostSelection")
    bool BeginGhostSelection(AController* Controller, float DurationSeconds);

    /**
     * 发起"幽灵重选"窗口（CF 式母体规格的用户定案：生化幽灵每次被枪械击杀复活后，
     * 有几秒机会重新选择幽灵角色；重选只换角色，进化等级不受影响；超时不选=保留当前角色）。
     *
     * 与 BeginGhostSelection（首次变身）的三处差异（都是语义决定，不是实现偷懒）：
     *  1) 阵营校验相反：必须**已是生化幽灵**（重选发生在幽灵复活之后）；
     *  2) **不卸 Avatar**：玩家带着当前角色照常行动，弹框是非阻塞的"机会窗口"；
     *  3) 超时行为不同：经状态机的重选模式位，超时=放弃并保留当前角色（首次变身则是强制保底）。
     *
     * 调用时机：由 Lua 规则在"幽灵复活完成"后调用（GameMode.lua 的 HandlePlayerRespawned，
     *           经角色 OnControllerReady 的服务器分支触发），保证窗口开在新 Avatar 就绪之后。
     *
     * @param Controller       要重选的玩家控制器（必须当前为生化幽灵阵营）
     * @param DurationSeconds  重选窗口秒数（规则数值，由 Lua CONFIG 提供，可热更）
     * @return 是否成功进入重选窗口（非幽灵 / 已在选择中 / 缺组件返回 false）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|GhostSelection")
    bool ReopenGhostSelection(AController* Controller, float DurationSeconds);

    /** 变身完成事件（BlueprintImplementableEvent）：某玩家已按其选择（或超时保底）变身为指定幽灵。
     *  UnLua 规则脚本实现同名函数 OnGhostTransformed 即自动接管（播报/特效/音效等连锁表现）。 */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|GhostSelection")
    void OnGhostTransformed(AController* Controller, TSubclassOf<AGameCharacterBase> GhostClass);

    /** 取"变身选择超时/上报非法"时的保底角色类（内容侧在 GameMode 蓝图配置，通常为基础幽灵） */
    TSubclassOf<AGameCharacterBase> GetDefaultGhostClass() const { return DefaultGhostClass; }

    // -------------------------------------------------------------------------
    // 攻击命中上报（感染规则的触发入口）
    // 说明：受击方角色在本端收到伤害后调用 ReportIncomingAttack 把"被命中"上报给 GameMode；
    //       本类只转发为 Lua 事件，是否感染/感染成何种幽灵等规则由 Lua 判定（可热更）。
    // -------------------------------------------------------------------------
    /**
     * 上报一次"某玩家被攻击命中"事件（服务器权威）。
     * 调用方：受害角色脚本（GameCharacterBase.lua）在服务器受击后调用，携带受击控制器、
     *         攻击来源 Actor 与本次伤害值；本函数只做权威校验与转发，不做任何规则判断。
     * @param VictimController 受击玩家的控制器（用于定位受害者与后续变异重生）
     * @param InstigatorActor  攻击来源 Actor（通常为发起攻击的 Pawn；可为空）
     * @param DamageAmount     本次伤害值（透传给规则脚本，供其做数值门槛判断等）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Infection")
    void ReportIncomingAttack(AController* VictimController, AActor* InstigatorActor, float DamageAmount);

    /**
     * 攻击命中落地事件（BlueprintImplementableEvent）。
     * UnLua 规则脚本（GameMode.lua）实现同名函数 OnIncomingAttackLanded 即自动接管；
     * 由 C++ 在 ReportIncomingAttack 校验通过后调用。规则示例：受击者为人类且攻击者为生化幽灵 → 感染。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Infection")
    void OnIncomingAttackLanded(AController* VictimController, AActor* InstigatorActor, float DamageAmount);

    // -------------------------------------------------------------------------
    // 死亡上报与处置（机制在 C++、规则在 Lua）
    // -------------------------------------------------------------------------
    /**
     * 上报一名玩家死亡（服务器权威；由死亡角色的 C++ 桥接回调调用）。
     * 本函数只做两件事：校验权威 → 派发 OnPlayerDied 事件交 Lua 规则脚本决定后续
     * （延迟复活 / 转观察者 / 击杀计分）。
     *
     * 注意：这里刻意不卸下/销毁死亡 Avatar —— 同一帧内"攻击命中即感染"的上报还需要通过角色
     *       读取其控制器（引擎的死亡回调早于受击回调）；"把尸体移出战场"由规则动作完成
     *       （ScheduleRespawn 与 SetPlayerAsSpectator 各自负责）。
     * @param VictimController 死亡玩家的控制器
     * @param KillerActor      击杀者（未知则为空；供规则做计分/结算）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Death")
    void ReportPlayerDeath(AController* VictimController, AActor* KillerActor);

    /**
     * 死亡落地事件（BlueprintImplementableEvent）。
     * UnLua 规则脚本实现同名函数 OnPlayerDied 即自动接管；
     * 典型规则：生化幽灵被击杀 → ScheduleRespawn（延迟复活仍为幽灵）；人类死亡 → SetPlayerAsSpectator。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Death")
    void OnPlayerDied(AController* VictimController, AActor* KillerActor);

    /**
     * 安排"延迟复活"（服务器权威）：先卸下并销毁该玩家当前的（死亡）Avatar，再在 DelaySeconds 秒后
     * 按其"当前阵营"重新生成（因此"僵尸死而复生仍是僵尸"）。
     * 分工说明：卸下/计时/生成是"机制"（放 C++ 以避免各端自行计时不一致）；
     *           而"延迟多少秒、要不要复活"是"规则"，由 Lua 传入参数决定（可热更调参）。
     * @param Controller   目标玩家控制器
     * @param DelaySeconds 复活延迟（秒；<=0 表示立即复活）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Respawn")
    void ScheduleRespawn(AController* Controller, float DelaySeconds);

    /**
     * 把玩家转为观察者（服务器权威）：标记观战状态 → 卸下并销毁当前 Avatar → 由控制器进入观察者状态。
     * 用于"人类死亡（如摔死）"这类不再参与本回合战斗的情形；
     * 回合结束时的重置流程会把观察者重新拉回战场（见 Lua 规则脚本的 ResetAllPlayersToHuman）——
     * 复活走"重新附身"路径，控制器会在其中自动收尾观战相机，无需调用方额外处理。
     *
     * 分工说明：本函数负责"服务器权威的状态与卸载"；真正生成观察者 Pawn、接管观战相机的是
     *           AGamePlayerController::EnterSpectatorMode（相机与视图目标是控制器级概念）。
     */
    UFUNCTION(BlueprintCallable, Category = "Match|Death")
    void SetPlayerAsSpectator(AController* Controller);

    // -------------------------------------------------------------------------
    // GM 调试命令派发（服务器控制台 "GK <命令> [参数]"；入口在 GM 服务、命令行为全在 Lua）
    // -------------------------------------------------------------------------
    /**
     * 派发一条 GM 命令给规则脚本（服务器权威；由 UGameGMSubsystem 解析控制台命令后调用）。
     * 分工：控制台入口注册/参数解析/权威世界查找在 GM 服务（C++ 薄机制），
     *       本函数只做"权威复核 + 转发 Lua"；命令行为（跳阶段/感染/改等级…）全部实现在
     *       GameMode.lua 的 GM_COMMANDS 表 —— 新增或修改命令不需要重编译 C++。
     * @param CommandId 命令 ID（如 "SkipPhase" / "Infect"）
     * @param Args      参数串（空格分隔；由 Lua 自行切分与解释）
     */
    UFUNCTION(BlueprintCallable, Category = "Match|GM")
    void DispatchGmCommand(const FString& CommandId, const FString& Args);

    /**
     * GM 命令落地事件（BlueprintImplementableEvent）。
     * UnLua 规则脚本定义同名函数 OnGmCommand 即自动接管（见 GameMode.lua 的 GM_COMMANDS 表）。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|GM")
    void OnGmCommand(const FString& CommandId, const FString& Args);

protected:
    // -------------------------------------------------------------------------
    // 框架级生命周期（C++ 职责：只派发事件，不做规则判断）
    // -------------------------------------------------------------------------
    /** 玩家加入对局：C++ 派发事件，人数/开局等规则由 Lua 决定 */
    virtual void PostLogin(APlayerController* NewPlayer) override;

    /** 玩家离开对局：C++ 派发事件，回退/结算等规则由 Lua 决定 */
    virtual void Logout(AController* Exiting) override;

    /**
     * 重写"某控制器应生成哪个 Pawn 类"（引擎在 RestartPlayer → SpawnDefaultPawnFor 时调用）。
     * 优先级：
     *  1) PlayerState 上显式指定的 PendingPawnClass（感染/换人时写入）；
     *  2) 该玩家当前阵营在 FactionPawnClasses 中配置的默认角色类（开局按阵营生成）；
     *  3) 引擎常规默认（DefaultPawnClass）。
     */
    virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;

    /**
     * 玩家入场通知（BlueprintImplementableEvent）。
     * UnLua 绑定的 Lua 脚本中定义同名函数（OnPlayerEnteredMatch）即自动接管。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Match")
    void OnPlayerEnteredMatch(APlayerController* NewPlayer);

    /**
     * 玩家离场通知（BlueprintImplementableEvent）。
     * UnLua 绑定的 Lua 脚本中定义同名函数（OnPlayerLeftMatch）即自动接管。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Match")
    void OnPlayerLeftMatch(AController* Exiting);

    /** Actor 结束：清除全部"待复活"定时器，防止对局销毁后回调打到已失效的 GameMode 上 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /**
     * 延迟复活定时器回调（服务器）。
     * 说明：以"当前阵营"复活，因此"僵尸死而复生仍是僵尸"；若玩家在等待期间已被别处复活（如回合重置），
     *       本回调会检测到其已有 Pawn 而跳过，避免重复生成。
     */
    void HandleScheduledRespawn(TWeakObjectPtr<AController> Controller);

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    /** 绑定的 Lua 规则脚本模块路径（相对 Content/Script），可在蓝图子类中覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

    /**
     * 各阵营的默认角色类：开局与按阵营重生时据此生成对应角色。
     * 说明：这里配置的是"类"，具体数值（体型/血量/速度/技能）由该角色的角色定义资产提供，
     *       因此换角色外观/数值不需要改本表。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match|Spawn")
    TMap<EGameFaction, TSubclassOf<AGameCharacterBase>> FactionPawnClasses;

    /**
     * 变身选择的保底角色类（超时/上报非法时自动变成本角色，保证流程永不卡死）。
     * 内容侧配置为基础幽灵（免费保底角色，即玩家仓库默认拥有的"普通母体"）。
     * 未配置时的兜底：使用该玩家仓库快照的第一项；两者皆空则超时完成失败并告警（见 PlayerController）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Match|GhostSelection")
    TSubclassOf<AGameCharacterBase> DefaultGhostClass;

private:
    /**
     * 待复活玩家 → 定时器句柄（服务器权威）。
     * 用弱引用作键：玩家中途离场（Controller 被销毁）时不会阻止 GC，也便于清理。
     */
    TMap<TWeakObjectPtr<AController>, FTimerHandle> PendingRespawnTimers;
};
