#include "GamePlayerState.h"
#include "GameAbilitySystemComponent.h"   // 自定义 ASC（开启子对象复制）
#include "GameCharacterBase.h"            // 幽灵角色基类完整类型：变身选择仓库快照的判空/Contains
                                          // 会实例化 TSubclassOf::operator UClass*()（内部调
                                          // AGameCharacterBase::StaticClass），必须见完整定义
#include "HumanAttributeSet.h"            // 人类属性集
#include "ZombieAttributeSet.h"           // 生化幽灵属性集
#include "AbilitySystemComponent.h"       // GAS 核心 API
#include "GameplayAbilitySpec.h"          // FGameplayAbilitySpec（技能授予规格）
#include "Abilities/GameplayAbility.h"    // UGameplayAbility（技能基类）
#include "GameplayEffect.h"               // FGameplayEffectSpec（MakeOutgoingSpec 返回值的解引用）
#include "Net/UnrealNetwork.h"            // DOREPLIFETIME（阵营复制注册）
#include "ZombieKing.h"                   // LogZombieKing

AGamePlayerState::AGamePlayerState()
{
    // -------------------------------------------------------------------------
    // 1. 自定义 ASC 子类：构造期已打开"注册子对象列表复制"，这是属性集能网络同步的前提
    // -------------------------------------------------------------------------
    AbilitySystemComponent = CreateDefaultSubobject<UGameAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    // 组件随 PlayerState 一起复制（引擎要求：Owner 复制，组件才可能复制）
    AbilitySystemComponent->SetIsReplicated(true);

    // -------------------------------------------------------------------------
    // 2. 复制模式 Mixed（竞技网游标准配置）：
    //    拥有者收到完整技能/效果信息；其它客户端只收最小信息；属性与标签仍全网可见。
    // -------------------------------------------------------------------------
    AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

    // -------------------------------------------------------------------------
    // 3. 两个阵营的属性集都创建为"默认子对象"
    // -------------------------------------------------------------------------
    // 关键原因：默认子对象在服务器与客户端都以稳定名字存在，切换注册时可正常参与复制子对象列表；
    // 若改用运行期 NewObject 新建属性集，客户端无法按名字寻址该对象，属性将复制失败（恒为 0）。
    HumanAttributeSet  = CreateDefaultSubobject<UHumanAttributeSet>(TEXT("HumanAttributeSet"));
    ZombieAttributeSet = CreateDefaultSubobject<UZombieAttributeSet>(TEXT("ZombieAttributeSet"));

    // -------------------------------------------------------------------------
    // 4. 开局阵营：生化模式玩家固定以人类入场，感染后再切换
    // -------------------------------------------------------------------------
    CurrentFaction = EGameFaction::Human;
}

void AGamePlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    // 先让基类注册其自身复制成员
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // 当前阵营对全网复制：准星变色、友伤过滤、HUD 阵营提示都需要它在客户端可用
    DOREPLIFETIME(AGamePlayerState, CurrentFaction);

    // 观战状态对全网复制：
    //  - 服务器侧的回合重置/复活规则需要它（"谁是观察者"不能只靠"有没有 Pawn"来推断，
    //    因为复活瞬间会先有 Pawn 再清状态，且离场玩家的 Pawn 也可能为空）；
    //  - 各客户端需要它来把比分板/名单标成"观战中"。
    DOREPLIFETIME(AGamePlayerState, bIsSpectating);

    // 变身选择状态对全网复制：所有人都能看到"XX 正在选择变身"，且服务器侧规则链读它
    DOREPLIFETIME(AGamePlayerState, bAwaitingGhostSelection);

    // 回合战绩对全网复制：计分板/结算面板要在各客户端一致显示每个人的 K/D/伤害；
    // 三个字段共用 OnRep_PlayerStats 回调（任一到达即广播"战绩变了"，HUD 整体重取）
    DOREPLIFETIME(AGamePlayerState, KillCount);
    DOREPLIFETIME(AGamePlayerState, DeathCount);
    DOREPLIFETIME(AGamePlayerState, DamageDealt);

    // 进化等级对全网复制：升级带来的体型/模型变化要在各端一致渲染，HUD 要显示幽灵等级
    DOREPLIFETIME(AGamePlayerState, ZombieLevel);

    // 感染计数对全网复制：升级进度展示（复用战绩回调广播）
    DOREPLIFETIME(AGamePlayerState, RoundInfectionCount);
}

void AGamePlayerState::PostInitializeComponents()
{
    Super::PostInitializeComponents();

    // 注册初始属性集（必须在"复制就绪"之后）：
    //  - 构造期调用 AddSpawnedAttribute 时 IsReadyForReplication() 为 false，不会登记复制子对象；
    //  - 此处（组件已注册）在服务器登记，引擎随即通过 AddReplicatedSubObject 完成登记；
    //  - 客户端由引擎内置的 OnRep_SpawnedAttributes 自动同步。
    if (HasAuthority() && AbilitySystemComponent)
    {
        if (UAttributeSet* InitialAttributeSet = ResolveAttributeSetForFaction(CurrentFaction))
        {
            AbilitySystemComponent->AddSpawnedAttribute(InitialAttributeSet);
        }
    }
}

UAttributeSet* AGamePlayerState::ResolveAttributeSetForFaction(EGameFaction Faction) const
{
    // 约定：Zombie → 生化幽灵属性集；其余（Human / None）→ 人类属性集。
    // 之所以"同一时刻只注册一个"：两种属性集都继承自 UGameAttributeSet，共有 Health/MaxHealth，
    // 同时注册会形成重名属性，导致取值与聚合器混乱。
    if (Faction == EGameFaction::Zombie)
    {
        return ZombieAttributeSet;
    }
    return HumanAttributeSet;
}

UAbilitySystemComponent* AGamePlayerState::GetAbilitySystemComponent() const
{
    return AbilitySystemComponent;
}

void AGamePlayerState::SetGenericTeamId(const FGenericTeamId& InTeamID)
{
    // 阵营是唯一权威：把通用阵营 ID 逆映射为阵营后走统一的 SetPlayerFaction 路径，
    // 保证"通过通用阵营 ID 写入"与"通过阵营写入"最终落在同一状态上。
    SetPlayerFaction(TeamIdToGameFaction(InTeamID));
}

FGenericTeamId AGamePlayerState::GetGenericTeamId() const
{
    // 通用阵营 ID 由阵营派生，保证全项目（AI/准星/友伤）判定口径一致
    return GameFactionToTeamId(CurrentFaction);
}

void AGamePlayerState::SetPlayerFaction(EGameFaction NewFaction)
{
    // 仅服务器权威可切换阵营（客户端切换会被忽略，防止自选阵营作弊）
    if (!HasAuthority() || !AbilitySystemComponent)
    {
        return;
    }

    // 阵营未变化则短路，避免无意义的属性集切换与事件广播
    if (CurrentFaction == NewFaction)
    {
        return;
    }

    const EGameFaction OldFaction = CurrentFaction;
    CurrentFaction = NewFaction;

    // 切换已注册的属性集：先移除旧阵营属性集，再注册新阵营属性集。
    // RemoveSpawnedAttribute 会同时注销该属性集的复制子对象并清理属性聚合器（引擎已内建），
    // 因此切换后数值从新属性集重新起算，随后由"该角色的初始化 GE"灌入满值。
    UAttributeSet* OldAttributeSet = ResolveAttributeSetForFaction(OldFaction);
    UAttributeSet* NewAttributeSet = ResolveAttributeSetForFaction(NewFaction);

    if (OldAttributeSet && OldAttributeSet != NewAttributeSet)
    {
        AbilitySystemComponent->RemoveSpawnedAttribute(OldAttributeSet);
    }
    if (NewAttributeSet)
    {
        AbilitySystemComponent->AddSpawnedAttribute(NewAttributeSet);
    }

    // 服务器不会触发 OnRep（OnRep 仅在客户端生效），因此在此手动广播一次，
    // 保证本地监听者（GameMode/HUD/Lua）与远端同时收到阵营变化事件。
    OnPlayerFactionChanged.Broadcast(NewFaction, OldFaction);
}

void AGamePlayerState::OnRep_CurrentFaction(EGameFaction OldFaction)
{
    // 客户端收到阵营变化：广播事件驱动准星变色、阵营提示、感染演出等表现（由可热更的 Lua 侧订阅）
    OnPlayerFactionChanged.Broadcast(CurrentFaction, OldFaction);
}

void AGamePlayerState::SetIsSpectating(bool bNewSpectating)
{
    // 仅服务器权威可切换观战状态：客户端切换会被忽略，防止客户端自行"免死/跳过回合"
    if (!HasAuthority())
    {
        return;
    }

    // 状态未变化则短路，避免无意义的事件广播（巡检/复活流程可能重复写入同一个值）
    if (bIsSpectating == bNewSpectating)
    {
        return;
    }

    bIsSpectating = bNewSpectating;

    // 服务器不会收到自己的 RepNotify（OnRep 只在客户端触发），故在此手动广播一次，
    // 保证本地监听者（GameMode/HUD/Lua）与远端客户端同时收到观战状态变化。
    OnSpectatingChanged.Broadcast(bIsSpectating);
}

void AGamePlayerState::OnRep_IsSpectating()
{
    // 客户端收到观战状态变化：广播事件驱动表现层（HUD 切观战界面、比分板标记观战者）
    OnSpectatingChanged.Broadcast(bIsSpectating);
}

// =============================================================================
//  变身选择（状态权威；流程状态机在 AGamePlayerController）
// =============================================================================
void AGamePlayerState::SetAwaitingGhostSelection(bool bNewAwaiting)
{
    // 仅服务器权威：变身选择由 Lua 规则发起（选拔/感染），客户端无权自置该状态
    if (!HasAuthority())
    {
        return;
    }

    // 状态未变化则短路，避免无意义的事件广播
    if (bAwaitingGhostSelection == bNewAwaiting)
    {
        return;
    }

    bAwaitingGhostSelection = bNewAwaiting;

    // 服务器不会收到自己的 RepNotify（OnRep 只在客户端触发），故在此手动广播一次，
    // 保证本地监听者（GameMode/HUD/Lua）与远端客户端同时收到状态变化。
    OnGhostSelectionChanged.Broadcast(bAwaitingGhostSelection);
}

void AGamePlayerState::OnRep_AwaitingGhostSelection()
{
    // 客户端收到变身选择状态变化：广播事件驱动选择框 UI 与"XX 正在选择变身"提示
    OnGhostSelectionChanged.Broadcast(bAwaitingGhostSelection);
}

// =============================================================================
//  回合战绩（机制：写入校验 + 复制 + 变更广播；"何时计入"的规则在 Lua）
// =============================================================================
void AGamePlayerState::AddKill()
{
    // 仅服务器权威：战绩由 Lua 规则在服务器侧调用，客户端无权自增（防作弊）
    if (!HasAuthority())
    {
        return;
    }

    ++KillCount;

    // 服务器不会收到自己的 RepNotify（OnRep 只在客户端触发），故在此手动广播一次，
    // 保证 ListenServer 本端 HUD 与远端客户端同时收到战绩变化。
    OnPlayerStatsChanged.Broadcast();
}

void AGamePlayerState::AddDeath()
{
    // 仅服务器权威（同 AddKill）
    if (!HasAuthority())
    {
        return;
    }

    ++DeathCount;
    OnPlayerStatsChanged.Broadcast();
}

void AGamePlayerState::AddDamageDealt(int32 InDamage)
{
    // 仅服务器权威（同 AddKill）
    if (!HasAuthority())
    {
        return;
    }

    // 拒绝非正数：伤害只增不减，堵住"误用负数把战绩改小"的口子（规则侧取整后应恒 >0）
    if (InDamage <= 0)
    {
        return;
    }

    DamageDealt += InDamage;
    OnPlayerStatsChanged.Broadcast();
}

void AGamePlayerState::ResetRoundStats()
{
    // 仅服务器权威（同 AddKill）
    if (!HasAuthority())
    {
        return;
    }

    // 全部字段为 0 时短路：回合开局对"本就干净的玩家"重复调用不产生多余广播与复制
    if (KillCount == 0 && DeathCount == 0 && DamageDealt == 0
        && ZombieLevel == 0 && RoundInfectionCount == 0)
    {
        return;
    }

    KillCount   = 0;
    DeathCount  = 0;
    DamageDealt = 0;
    RoundInfectionCount = 0;
    OnPlayerStatsChanged.Broadcast();

    // 进化等级清零（回到人类形态）单独走 SetZombieLevel：
    // 复用其"变化才广播"的短路逻辑，并让角色侧订阅者收到"等级归 0"事件（重置体型/模型表现）
    SetZombieLevel(0);
}

void AGamePlayerState::OnRep_PlayerStats()
{
    // 客户端收到任一战绩字段到达：广播"战绩变了"，HUD 自行重取三个值刷新（事件不带参数）
    OnPlayerStatsChanged.Broadcast();
}

// =============================================================================
//  生化幽灵进化等级（状态权威；升级落地在角色侧 SetZombieLevel，规则在 Lua）
// =============================================================================
void AGamePlayerState::SetZombieLevel(int32 NewLevel)
{
    // 仅服务器权威：等级由 Lua 规则在服务器侧写入，客户端无权自置（防自选等级作弊）
    if (!HasAuthority())
    {
        return;
    }

    // 负数钳到 0（0 = 人类/未变身哨兵值）；上限不在此校验——"几级封顶"是规则，由 Lua 判定
    NewLevel = FMath::Max(0, NewLevel);

    // 状态未变化则短路，避免无意义的事件广播（回合重置流程可能对人类玩家重复写 0）
    if (ZombieLevel == NewLevel)
    {
        return;
    }

    ZombieLevel = NewLevel;

    // 服务器不会收到自己的 RepNotify（OnRep 只在客户端触发），故在此手动广播一次，
    // 保证 ListenServer 本端角色与远端客户端同时收到等级变化。
    OnZombieLevelChanged.Broadcast(ZombieLevel);
}

void AGamePlayerState::OnRep_ZombieLevel()
{
    // 客户端收到等级到达：广播事件 → 角色订阅者刷新体型/模型本地表现，HUD 播进化横幅
    OnZombieLevelChanged.Broadcast(ZombieLevel);
}

void AGamePlayerState::AddInfectionCount(int32 InCount)
{
    // 仅服务器权威（同 AddKill）
    if (!HasAuthority())
    {
        return;
    }

    // 拒绝非正数：计数只增不减（清零走 SetRoundInfectionCount，语义分离防误用）
    if (InCount <= 0)
    {
        return;
    }

    RoundInfectionCount += InCount;
    OnPlayerStatsChanged.Broadcast();
}

void AGamePlayerState::SetRoundInfectionCount(int32 NewCount)
{
    // 仅服务器权威（同 AddKill）
    if (!HasAuthority())
    {
        return;
    }

    NewCount = FMath::Max(0, NewCount);
    if (RoundInfectionCount == NewCount)
    {
        return;
    }

    RoundInfectionCount = NewCount;
    OnPlayerStatsChanged.Broadcast();
}

void AGamePlayerState::SetOwnedGhostClasses(const TArray<TSubclassOf<AGameCharacterBase>>& InGhostClasses)
{
    // 仅服务器：快照由 PC 的 Server RPC 收到客户端上报后写入（见 GamePlayerState.h 信任模型说明）
    if (!HasAuthority())
    {
        return;
    }

    OwnedGhostClasses = InGhostClasses;

    // 顺手滤掉空槽位（客户端仓库服务理论上不会给空类，这里兜底保证校验语义干净）
    OwnedGhostClasses.RemoveAll([](const TSubclassOf<AGameCharacterBase>& GhostClass)
    {
        return GhostClass == nullptr;
    });
}

bool AGamePlayerState::IsGhostOwned(TSubclassOf<AGameCharacterBase> GhostClass) const
{
    if (GhostClass == nullptr)
    {
        return false;
    }

    // 按类指针精确比对：快照与选择上报都以"同一 UClass 指针"为身份（索引上报最终也解析成类）
    return OwnedGhostClasses.Contains(GhostClass);
}

void AGamePlayerState::ApplyInitialAttributeSetEffect(TSubclassOf<UGameplayEffect> InEffect)
{
    // 仅服务器权威可施加初始化效果：客户端施加会被 GAS 忽略，且可能造成数值不一致
    if (!HasAuthority() || !AbilitySystemComponent)
    {
        return;
    }

    // 未指定初始化 GE 资产：无法灌入初始数值，显式告警便于第一时间定位配置遗漏
    if (!InEffect)
    {
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 角色定义未指定 InitialAttributeSetEffect：初始生命等数值不会被灌入"
                 "（请在角色定义资产 UGameCharacterDefinition 的 Attributes 分类下指定初始化 GameplayEffect）。"),
            *GetName());
        return;
    }

    // 构造效果上下文：把来源对象标记为本 PlayerState，
    // 便于后续伤害/治疗归因（AttributeSet 的 PostGameplayEffectExecute 可据此追溯来源）
    FGameplayEffectContextHandle ContextHandle = AbilitySystemComponent->MakeEffectContext();
    ContextHandle.AddSourceObject(this);

    // 生成效果规格并施加到自身：具体数值与修饰方式全部由内容侧 GE 资产决定（可热更）
    const FGameplayEffectSpecHandle SpecHandle =
        AbilitySystemComponent->MakeOutgoingSpec(InEffect, 1.0f, ContextHandle);

    if (SpecHandle.IsValid())
    {
        AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
    }
    else
    {
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 初始化属性 GameplayEffect 规格生成失败，请检查 InitialAttributeSetEffect 资产是否有效。"),
            *GetName());
    }
}

void AGamePlayerState::GrantCharacterAbilities(const TArray<TSubclassOf<UGameplayAbility>>& InAbilities)
{
    // 仅服务器权威可授予技能：客户端授予会被 GAS 忽略，且会造成各端技能不一致
    if (!HasAuthority() || !AbilitySystemComponent)
    {
        return;
    }

    // 先清旧技能：保证"技能集与当前角色严格一致"（换人 / 感染变异的核心保证）
    ClearCharacterAbilities();

    for (const TSubclassOf<UGameplayAbility>& AbilityClass : InAbilities)
    {
        // 空元素（蓝图数组里未填的槽位）直接跳过，避免崩溃
        if (!AbilityClass)
        {
            continue;
        }

        // SourceObject 传当前 Avatar（Pawn）：
        // 技能可通过它拿到角色本体、Mover 组件、表现载体等，是"技能作用于角色"的锚点。
        FGameplayAbilitySpec AbilitySpec(AbilityClass, /*Level=*/1, /*InputID=*/INDEX_NONE, GetPawn());

        // GiveAbility 的返回值是句柄，保存下来用于后续精确清除
        GrantedAbilityHandles.Add(AbilitySystemComponent->GiveAbility(AbilitySpec));
    }
}

void AGamePlayerState::ClearCharacterAbilities()
{
    if (!HasAuthority() || !AbilitySystemComponent)
    {
        return;
    }

    // 逐个按句柄清除：只清"本 PlayerState 授予的"技能，
    // 不会误伤系统或其它来源授予的技能（这是用句柄列表而非 ClearAllAbilities 的原因）。
    for (const FGameplayAbilitySpecHandle& AbilityHandle : GrantedAbilityHandles)
    {
        if (AbilityHandle.IsValid())
        {
            AbilitySystemComponent->ClearAbility(AbilityHandle);
        }
    }

    GrantedAbilityHandles.Reset();
}
