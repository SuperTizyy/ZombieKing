#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "GenericTeamAgentInterface.h"
#include "GameplayAbilitySpecHandle.h"    // FGameplayAbilitySpecHandle（已授予技能的句柄）
#include "GameFaction.h"                 // EGameFaction（阵营）与阵营↔通用阵营 ID 映射
#include "GamePlayerState.generated.h"

class UGameAbilitySystemComponent;   // 自定义 ASC 子类（打开属性集复制）
class UAttributeSet;                 // 属性集基类（ResolveAttributeSetForFaction 返回类型）
class UHumanAttributeSet;            // 人类属性集
class UZombieAttributeSet;           // 生化幽灵属性集
class UGameplayEffect;               // 初始化属性效果
class UGameplayAbility;              // 技能基类（用于授予列表的元素类型）
class APawn;                         // Pawn（"下次重生的 Pawn 类"元素类型）
class AGameCharacterBase;            // 幽灵角色基类（变身选择"幽灵仓库快照"的元素类型）

/** 阵营变化广播（供 UI / Lua 表现层订阅：准星变色、阵营提示、感染演出等） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerFactionChanged, EGameFaction, NewFaction, EGameFaction, OldFaction);

/** 观战状态变化广播（供 UI / Lua 表现层订阅：切观战 HUD、比分板标记"观战中"等） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerSpectatingChanged, bool, bSpectating);

/**
 * 变身选择状态变化广播（true = 该玩家正在"变身选择框"中挑选幽灵角色）。
 * 供 HUD（WBP/Lua）展示"XX 正在选择变身"、以及选择框自身的开关驱动。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerGhostSelectionChanged, bool, bAwaitingSelection);

/**
 * 回合战绩变化广播（无参数）。
 * 击杀/死亡/伤害/感染计数任一数值变化时广播（服务器写入后本地广播、客户端 OnRep 时广播）；
 * 订阅方（HUD 计分板 / 结算面板）收到后自行调 GetKillCount/GetDeathCount/GetDamageDealt/GetRoundInfectionCount 取值刷新，
 * 事件不带参数以避免复制多余数据。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnPlayerStatsChanged);

/**
 * 生化幽灵进化等级变化广播（携带新等级；0 = 非幽灵/未进化）。
 * 服务器在 SetZombieLevel 后本地广播、客户端在 OnRep 时广播；
 * 角色（当前 Avatar）订阅后刷新"体型缩放 + 模型替换"等本地表现，HUD 订阅后播进化横幅。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerZombieLevelChanged, int32, NewLevel);

/**
 * 竞技生化网游：玩家状态类（GAS 的长期托管者 + 感染/变异的状态权威）
 *
 * 架构定位（工业级 GAS 规范）：
 *  - PlayerState 是跨死亡/变异存活的长期对象，作为 GAS 的 OwnerActor 托管 ASC、属性集与技能；
 *  - 角色的 Pawn（AGameCharacterBase）仅作为短生命周期的 AvatarActor，
 *    感染变异/换人时只需切换 Avatar 与属性集，不重建 PlayerState / ASC
 *    （避免 GAS 状态丢失与回调悬空，这是把 ASC 放在 PlayerState 而不是 Pawn 的根本原因）。
 *
 * 感染/变异链路（本类负责"状态"部分）：
 *  1) SetPlayerFaction(Zombie) —— 切换阵营：更新通用阵营 ID + 切换已注册的属性集（人类 ↔ 生化）；
 *  2) 由 GameMode 重新生成生化幽灵 Avatar（见 AGameGameMode::InfectPlayer）；
 *  3) 新 Avatar 附身后，AGameCharacterBase::SetupAbilitySystemContext 会按"该角色定义"灌入初始属性
 *     并授予其技能集 —— 于是变异后血量/技能自动变为僵尸那一套。
 *
 * TODO（后续里程碑）：技能输入绑定（Enhanced Input → 技能输入 Tag → TryActivateAbility）。
 */
UCLASS()
class ZOMBIEKING_API AGamePlayerState : public APlayerState,
                                         public IAbilitySystemInterface,
                                         public IGenericTeamAgentInterface
{
    GENERATED_BODY()

public:
    AGamePlayerState();

    /** 网络复制注册（复制当前阵营到客户端，驱动准星变色 / 友伤过滤 / UI） */
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** 组件初始化完成后登记属性集（必须在"复制就绪"之后才能进入复制子对象列表） */
    virtual void PostInitializeComponents() override;

    // -------------------------------------------------------------------------
    // IAbilitySystemInterface
    // -------------------------------------------------------------------------
    /** 返回本 PlayerState 托管的 GAS 能力系统组件 */
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

    /** 取自定义 ASC 类型（供需要 ASC 级扩展接口的调用方使用，避免上层到处 Cast） */
    UGameAbilitySystemComponent* GetGameAbilitySystemComponent() const { return AbilitySystemComponent; }

    // -------------------------------------------------------------------------
    // IGenericTeamAgentInterface（阵营唯一数据源）
    // 说明：阵营以 EGameFaction 为准，通用阵营 ID 由 GameFactionToTeamId 派生，保证前端各系统口径一致。
    // -------------------------------------------------------------------------
    virtual void SetGenericTeamId(const FGenericTeamId& InTeamID) override;
    virtual FGenericTeamId GetGenericTeamId() const override;

    // -------------------------------------------------------------------------
    // 阵营（感染/变异的状态权威）
    // -------------------------------------------------------------------------
    /** 阵营变化事件（服务器本地与客户端 OnRep 都会广播） */
    UPROPERTY(BlueprintAssignable, Category = "Faction")
    FOnPlayerFactionChanged OnPlayerFactionChanged;

    /** 当前阵营 */
    UFUNCTION(BlueprintPure, Category = "Faction")
    EGameFaction GetPlayerFaction() const { return CurrentFaction; }

    /**
     * 切换到指定阵营（服务器权威；客户端调用会被安全忽略）。
     * 内部动作：更新阵营与通用阵营 ID → 切换已注册的属性集（人类 ↔ 生化）→ 广播阵营变化事件。
     * 注意：本函数只切换"状态"，不负责重新生成 Avatar（那是 GameMode 职责，见 AGameGameMode::InfectPlayer）。
     */
    UFUNCTION(BlueprintCallable, Category = "Faction")
    void SetPlayerFaction(EGameFaction NewFaction);

    // -------------------------------------------------------------------------
    // 观战状态（服务器权威；观战相机由 PlayerController 侧执行，本类只承载"状态"）
    // -------------------------------------------------------------------------
    /**
     * 观战状态变化事件。
     * 服务器在 SetIsSpectating 后本地广播、客户端在 OnRep_IsSpectating 中广播，
     * 供 HUD（WBP/Lua）切换"观战 HUD / 战斗 HUD"以及比分板标记观战者。
     */
    UPROPERTY(BlueprintAssignable, Category = "Spectator")
    FOnPlayerSpectatingChanged OnSpectatingChanged;

    /** 本玩家当前是否处于观战状态（"本回合不再参与战斗"，与引擎自己的 bOnlySpectator 语义不同） */
    UFUNCTION(BlueprintPure, Category = "Spectator")
    bool IsSpectating() const { return bIsSpectating; }

    /**
     * 设置观战状态（服务器权威；客户端调用会被安全忽略）。
     *
     * 分工说明：本函数只切"状态"并复制给全网；
     * 真正的"进入/退出观察者状态（生成观察者 Pawn、接管观战相机）"由
     * AGamePlayerController::EnterSpectatorMode / 重新附身路径负责 —— 那属于控制器职责。
     */
    UFUNCTION(BlueprintCallable, Category = "Spectator")
    void SetIsSpectating(bool bNewSpectating);

    // -------------------------------------------------------------------------
    // 变身选择（CF 式"变成幽灵时从仓库挑角色"；状态权威在此，流程状态机在 PlayerController）
    // -------------------------------------------------------------------------
    /**
     * 变身选择状态变化事件。
     * 服务器在 SetAwaitingGhostSelection 后本地广播、客户端在 OnRep 中广播，
     * 供选择框 UI 与"XX 正在选择变身"提示驱动。
     */
    UPROPERTY(BlueprintAssignable, Category = "Ghost")
    FOnPlayerGhostSelectionChanged OnGhostSelectionChanged;

    /** 本玩家当前是否正在"变身选择"中（待选幽灵角色；复制给全网供 UI 展示） */
    UFUNCTION(BlueprintPure, Category = "Ghost")
    bool IsAwaitingGhostSelection() const { return bAwaitingGhostSelection; }

    /**
     * 设置变身选择状态（服务器权威；客户端调用会被安全忽略）。
     * 只切"状态"并复制；选择框弹出、倒计时、上报与超时全部由 AGamePlayerController 的状态机执行。
     */
    UFUNCTION(BlueprintCallable, Category = "Ghost")
    void SetAwaitingGhostSelection(bool bNewAwaiting);

    // -------------------------------------------------------------------------
    // 回合战绩（击杀 / 死亡 / 造成伤害；每回合清零，跨回合累计属后续"成长档案"里程碑）
    //
    // 分工说明（热更新铁律的直接落点）：
    //  - 本类只提供"战绩数据 + 复制 + 变更广播"的机制；
    //  - "什么算一次击杀/死亡、伤害何时计入"是**规则**，由 Lua 规则脚本在既有上报点
    //    （GameMode.lua 的 OnIncomingAttackLanded / OnPlayerDied）调用下方接口完成，
    //    与击杀播报（KillFeed）共用同一套判定口径，保证"播报数 == 战绩数"不打架。
    // -------------------------------------------------------------------------
    /**
     * 战绩变化事件（服务器写入后本地广播、客户端 OnRep 时广播）。
     * 供 HUD 计分板 / 结算面板订阅，收到后自行读 GetKillCount/GetDeathCount/GetDamageDealt。
     */
    UPROPERTY(BlueprintAssignable, Category = "Stats")
    FOnPlayerStatsChanged OnPlayerStatsChanged;

    /** 本回合击杀数（只读；随复制到达各端，客户端看到的即服务器权威值） */
    UFUNCTION(BlueprintPure, Category = "Stats")
    int32 GetKillCount() const { return KillCount; }

    /** 本回合死亡数（含"被感染倒下"——被生化打死同样记一次死亡，与 CF 口径一致） */
    UFUNCTION(BlueprintPure, Category = "Stats")
    int32 GetDeathCount() const { return DeathCount; }

    /** 本回合累计造成的伤害（按实际生效伤害取整累计，来源=攻击者侧结算结果） */
    UFUNCTION(BlueprintPure, Category = "Stats")
    int32 GetDamageDealt() const { return DamageDealt; }

    /** 击杀数 +1（仅服务器生效；由 Lua 在"确认击杀成立"时调用） */
    UFUNCTION(BlueprintCallable, Category = "Stats")
    void AddKill();

    /** 死亡数 +1（仅服务器生效；由 Lua 在"玩家死亡处置"入口调用，含被感染倒下） */
    UFUNCTION(BlueprintCallable, Category = "Stats")
    void AddDeath();

    /** 累计造成伤害（仅服务器生效；负数与 0 被拒绝，防止误用把战绩改小） */
    UFUNCTION(BlueprintCallable, Category = "Stats")
    void AddDamageDealt(int32 InDamage);

    /** 清空本回合战绩（仅服务器生效；回合开局由 Lua 统一调用） */
    UFUNCTION(BlueprintCallable, Category = "Stats")
    void ResetRoundStats();

    // -------------------------------------------------------------------------
    // 生化幽灵进化等级（CF 式 6 级轨：母体 3 级起步、被感染者 1 级起步，感染计数升级）
    //
    // 分工说明（热更新铁律）：
    //  - 本类只承载"等级与感染计数的状态 + 复制 + 变化广播"（机制）；
    //  - "升 1 级需要感染几人、谁从几级起步、上限几级"全部是**规则**，写在 Lua 规则脚本
    //    （GameMode.lua 的 OnIncomingAttackLanded / OnGhostTransformed）；
    //  - 升级的"属性增量 + 体型/模型变化"落地在角色侧（AGameCharacterBase::SetZombieLevel，
    //    数值表在 UGameCharacterDefinition::ZombieLevels 资产），本类不做任何结算。
    // -------------------------------------------------------------------------
    /** 进化等级变化事件（服务器写入后本地广播、客户端 OnRep 时广播；携带新等级） */
    UPROPERTY(BlueprintAssignable, Category = "ZombieEvolution")
    FOnPlayerZombieLevelChanged OnZombieLevelChanged;

    /** 当前进化等级（0 = 人类/未变身；1..6 = 幽灵等级；随复制到达各端） */
    UFUNCTION(BlueprintPure, Category = "ZombieEvolution")
    int32 GetZombieLevel() const { return ZombieLevel; }

    /**
     * 设置进化等级（仅服务器生效；客户端调用被安全忽略，防自选等级作弊）。
     * 只写状态并广播；"升级带来的血量/体型/模型变化"由角色侧 SetZombieLevel 统一落地。
     */
    UFUNCTION(BlueprintCallable, Category = "ZombieEvolution")
    void SetZombieLevel(int32 NewLevel);

    /** 本回合累计"感染命中"计数（升级进度；随升级清零重新累计，口径由 Lua 规则决定） */
    UFUNCTION(BlueprintPure, Category = "ZombieEvolution")
    int32 GetRoundInfectionCount() const { return RoundInfectionCount; }

    /** 增加感染计数（仅服务器生效；Lua 在"感染成功"时调用；非正数被拒绝） */
    UFUNCTION(BlueprintCallable, Category = "ZombieEvolution")
    void AddInfectionCount(int32 InCount);

    /** 直接设置感染计数（仅服务器生效；Lua 升级后清零进度用） */
    UFUNCTION(BlueprintCallable, Category = "ZombieEvolution")
    void SetRoundInfectionCount(int32 NewCount);

    // -------------------------------------------------------------------------
    // 幽灵仓库快照（服务器侧；进对局时由客户端上报，用于"所选角色必须已拥有"的防作弊校验）
    // -------------------------------------------------------------------------
    /**
     * 服务器侧写入本玩家的幽灵仓库快照（由 AGamePlayerController 在收到客户端上报后调用）。
     *
     * 信任模型说明（当前阶段的明确取舍）：
     *  - 仓库真值在客户端本地（无后端账号体系），快照由客户端上报 —— "选了未拥有的角色"无法被
     *    服务器完全证伪（清单本身可伪造）；但"选择 ∈ 快照"这一层校验仍拦住了"凭空选任意类"
     *    的裸作弊面（选到未配置/未生成的角色类）。
     *  - 后端账号体系就绪后：仓库权威迁移到后端，服务器从后端拉取清单，快照上报路径整体退役。
     */
    void SetOwnedGhostClasses(const TArray<TSubclassOf<AGameCharacterBase>>& InGhostClasses);

    /** 查询服务器侧仓库快照（仅服务器有意义；客户端恒为空） */
    const TArray<TSubclassOf<AGameCharacterBase>>& GetOwnedGhostClasses() const { return OwnedGhostClasses; }

    /** 该角色类是否在本玩家的仓库快照中（服务器校验用；空类不算拥有） */
    bool IsGhostOwned(TSubclassOf<AGameCharacterBase> GhostClass) const;

    // -------------------------------------------------------------------------
    // 下一次重生的 Pawn 类（感染/换人时由 GameMode 指定，服务器权威、无需复制）
    // -------------------------------------------------------------------------
    /** 指定"下次重生应使用的 Pawn 类"；GameMode 的 GetDefaultPawnClassForController 会读取它 */
    void SetPendingPawnClass(TSubclassOf<APawn> InPawnClass) { PendingPawnClass = InPawnClass; }

    /** 读取"下次重生应使用的 Pawn 类"（未指定时为空） */
    TSubclassOf<APawn> GetPendingPawnClass() const { return PendingPawnClass; }

    // -------------------------------------------------------------------------
    // 初始属性灌入
    // -------------------------------------------------------------------------
    /**
     * 服务器端：向自身施加"初始化属性"GameplayEffect，灌入初始生命等数值。
     * @param InEffect 初始化效果类：来自角色定义（UGameCharacterDefinition）——每种角色类型一份资产，
     *                 因此"每只生化幽灵血量不同"只需改资产，不必重编译 C++（满足热更新要求）。
     * 由 AGameCharacterBase::SetupAbilitySystemContext 在 Avatar 绑定后调用。
     */
    void ApplyInitialAttributeSetEffect(TSubclassOf<UGameplayEffect> InEffect);

    // -------------------------------------------------------------------------
    // 技能授予（服务器权威）
    // -------------------------------------------------------------------------
    /**
     * 授予当前 Avatar（角色）声明的默认技能集。
     * 会先清除本 PlayerState 先前授予的技能，保证技能集始终与当前角色一致（换人/变异场景）。
     */
    void GrantCharacterAbilities(const TArray<TSubclassOf<UGameplayAbility>>& InAbilities);

    /** 清除本 PlayerState 授予的全部技能（只清自己授予的，不动他人/系统授予的技能） */
    void ClearCharacterAbilities();

protected:
    /** 阵营复制回调（仅客户端触发）：广播阵营变化事件，驱动表现层刷新 */
    UFUNCTION()
    virtual void OnRep_CurrentFaction(EGameFaction OldFaction);

    /** 观战状态复制回调（仅客户端触发）：广播观战状态变化事件，驱动 HUD 切换观战界面 */
    UFUNCTION()
    virtual void OnRep_IsSpectating();

    /** 变身选择状态复制回调（仅客户端触发）：广播变身选择状态事件，驱动选择框 UI 与提示 */
    UFUNCTION()
    virtual void OnRep_AwaitingGhostSelection();

    /**
     * 战绩复制回调（仅客户端触发）：三个战绩字段共用同一个回调，
     * 任一数值到达都会广播 OnPlayerStatsChanged（HUD 收到后整体重取三个值，无需知道"哪一个变了"）。
     */
    UFUNCTION()
    virtual void OnRep_PlayerStats();

    /** 进化等级复制回调（仅客户端触发）：广播 OnZombieLevelChanged，角色/HUD 据此刷新表现 */
    UFUNCTION()
    virtual void OnRep_ZombieLevel();

    /**
     * 按阵营解析应注册的属性集。
     * 约定：Zombie → 生化幽灵属性集；其余（Human/None）→ 人类属性集。
     * 之所以"同一时刻只注册一个阵营属性集"：两种属性集都继承自 UGameAttributeSet，
     * 共有 Health/MaxHealth；若同时注册会出现重名属性，导致 GAS 取值与聚合器混乱。
     */
    UAttributeSet* ResolveAttributeSetForFaction(EGameFaction Faction) const;

protected:
    /** GAS 能力系统组件（自定义子类，构造期已开启属性集复制） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
    TObjectPtr<UGameAbilitySystemComponent> AbilitySystemComponent;

    /**
     * 人类属性集（默认子对象）。
     * 说明：两个阵营的属性集都以"默认子对象"形式在构造期创建 ——
     *       这样它们在服务器与客户端都以稳定名字存在，切换注册时可正常参与复制；
     *       而"运行期 NewObject 新建属性集"会因对象在客户端无法寻址而复制失败。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
    TObjectPtr<UHumanAttributeSet> HumanAttributeSet;

    /** 生化幽灵属性集（默认子对象；未注册前仅作为对象存在，不参与 GAS 取值） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
    TObjectPtr<UZombieAttributeSet> ZombieAttributeSet;

    /**
     * 当前阵营（复制到客户端）。
     * 生化模式开盘玩家固定为人类，故默认 Human；感染后由 SetPlayerFaction 切为 Zombie。
     */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CurrentFaction, Category = "Faction")
    EGameFaction CurrentFaction = EGameFaction::Human;

    /**
     * 是否处于观战状态（复制到客户端）。
     * 语义：该玩家"本回合不再参与战斗"（典型场景：人类死亡转观察者）；
     * 之所以要复制而不是只放本机：回合重置/复活流程、比分板、观战 HUD 都需要在服务器与各客户端一致地看到该状态。
     * 渲染观战相机所需的数据（当前看谁/相机模式）则在 AGamePlayerController 的观战组件里，不需要复制。
     */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_IsSpectating, Category = "Spectator")
    bool bIsSpectating = false;

    /**
     * 是否正在"变身选择"中（复制给全网）。
     * 复制的理由：所有人都要能在 HUD 上看到"XX 正在选择变身"；且服务器侧的
     * 胜负判定/重复感染拦截也要读它（选择期间该玩家阵营已是 Zombie、但没有 Pawn）。
     */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AwaitingGhostSelection, Category = "Ghost")
    bool bAwaitingGhostSelection = false;

    /**
     * 本回合击杀数（复制到客户端）。
     * 复制的理由：计分板/结算面板要在所有客户端一致显示每个人的战绩；
     * 写入权只在服务器（AddKill/ResetRoundStats 内部均有 HasAuthority 校验）。
     */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PlayerStats, Category = "Stats")
    int32 KillCount = 0;

    /** 本回合死亡数（复制到客户端；写入纪律同 KillCount） */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PlayerStats, Category = "Stats")
    int32 DeathCount = 0;

    /** 本回合累计造成伤害（复制到客户端；写入纪律同 KillCount） */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PlayerStats, Category = "Stats")
    int32 DamageDealt = 0;

    /**
     * 生化幽灵进化等级（复制到客户端；0 = 人类/未变身，1..6 = 幽灵等级）。
     * 复制的理由：升级带来的体型缩放与模型替换要在各端一致渲染（碰撞体积是玩法的一部分，
     * 受击判定/遮挡都依赖它）；HUD 也要显示"几级幽灵"。
     * 写入权只在服务器（SetZombieLevel 有 HasAuthority 校验）。
     */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ZombieLevel, Category = "ZombieEvolution")
    int32 ZombieLevel = 0;

    /** 本回合感染计数（复制到客户端：升级进度条/比分板展示；写入纪律同 ZombieLevel，随战绩一起清零） */
    UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_PlayerStats, Category = "ZombieEvolution")
    int32 RoundInfectionCount = 0;

    /**
     * 幽灵仓库快照（服务器侧，不复制：客户端选择框读的是自己本地的仓库服务）。
     * 由 AGamePlayerController 收到客户端上报后写入；"选择 ∈ 快照"的服务器校验数据源。
     */
    UPROPERTY()
    TArray<TSubclassOf<AGameCharacterBase>> OwnedGhostClasses;

    /**
     * 下一次重生应使用的 Pawn 类（服务器权威，不复制：只有服务器负责生成 Pawn）。
     * 感染/换人时由 GameMode 写入，随后 GameMode 的 GetDefaultPawnClassForController 读取它。
     */
    UPROPERTY()
    TSubclassOf<APawn> PendingPawnClass;

    /**
     * 已授予技能的句柄列表：
     * 用于在离身/换人时"精确清除本 PlayerState 授予的技能"，
     * 避免误清系统或其它来源授予的技能。
     */
    TArray<FGameplayAbilitySpecHandle> GrantedAbilityHandles;
};
