#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GameAbilitySystemComponent.generated.h"

class AActor;
class UGameplayEffect;

/**
 * 命中确认广播（发回攻击者本端）。
 * 用途：HUD 命中标记（HitMarker）、命中音效——这些反馈只有"攻击者自己"需要看到，
 *       且必须由服务器确认（客户端无法凭自己判断是否真的打中了）。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnGameHitConfirmed, AActor*, HitTarget, float, DamageAmount, bool, bHeadshot);

/**
 * 击杀确认广播（发回击杀者本端）。
 * 用途：击杀提示（"你击杀了 X"）、击杀音效、连杀计数（后续里程碑）。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameKillConfirmed, AActor*, VictimActor);

/**
 * 竞技生化网游：自定义能力系统组件（ASC）
 *
 * 为什么必须自定义 ASC 子类（引擎事实，非风格偏好）：
 *  - GAS 的 AttributeSet 属性要能网络同步，前提是 ASC 打开"注册子对象列表复制"
 *    （UActorComponent::bReplicateUsingRegisteredSubObjectList）。该成员是 protected，
 *    外部类无法直接改写，只能在 UActorComponent 的子类构造期设置；
 *  - 且引擎默认值为 false（ActorComponent.cpp 中 GDefaultUseSubObjectReplicationList = false），
 *    不打开则 AddSpawnedAttribute 只把属性集塞进数组、不会登记为复制子对象，
 *    客户端永远拿不到生命值（UI 恒为 0）。
 *
 * 本类承载 ASC 级的公共框架能力（当前：伤害施加；后续：技能授予辅助、GameplayCue 等）。
 */
UCLASS()
class ZOMBIEKING_API UGameAbilitySystemComponent : public UAbilitySystemComponent
{
    GENERATED_BODY()

public:
    UGameAbilitySystemComponent();

    // -------------------------------------------------------------------------
    // 伤害施加（服务器权威）
    // -------------------------------------------------------------------------
    /**
     * 对目标施加一次伤害（服务器权威；客户端调用会被安全忽略）。
     *
     * 设计说明：
     *  - "伤害数值"通过 SetByCaller(Data.Damage) 传入，因此同一份伤害 GameplayEffect 资产
     *    可服务所有伤害来源（近战、子弹、技能）——改数值不必换资产、不必重编译；
     *  - 实际结算由伤害 GE 引用的 UGameDamageExecution 完成（服务器侧执行计算）；
     *  - 归因信息（Source/Instigator）会写入效果上下文，供 AttributeSet 与 HealthComponent
     *    追溯"谁打的"，进而驱动击杀结算与死亡演出。
     *
     * @param TargetActor        受击目标（需实现 IAbilitySystemInterface）
     * @param DamageAmount       原始伤害值（<=0 视为无效，直接返回 false）
     * @param DamageEffectClass  伤害效果类（内容资产，需引用 UGameDamageExecution 作为执行计算）
     * @param DamageSource       伤害来源（用于归因，通常为发起攻击的 Pawn；可为空）
     * @param bMeleeDamage       是否为近战（刀/爪）伤害：为 true 时在效果规格上附加动态资产标签
     *                           Damage.Source.Melee，受击方据此缓存"最近一次受击是否近战"，
     *                           供 Lua 死亡规则实现 CF 式"刀杀禁令"（被刀杀的幽灵本回合禁复活）。
     * @return 是否成功施加（目标无 ASC、免疫等情况返回 false）
     */
    UFUNCTION(BlueprintCallable, Category = "Damage")
    bool ApplyDamageToTarget(AActor* TargetActor, float DamageAmount, TSubclassOf<UGameplayEffect> DamageEffectClass, AActor* DamageSource, bool bMeleeDamage = false);

    /**
     * 带命中信息的伤害施加（C++ 专用重载，不做 BlueprintCallable）。
     * 之所以单独一个重载而没有并进上面的 UFUNCTION：FHitResult 不便由 Lua 构造，
     * Lua 侧使用不带命中的版本即可；而 C++ 侧（射击/近战命中逻辑）需要把命中细节带进效果上下文。
     */
    bool ApplyDamageToTargetWithHit(AActor* TargetActor, float DamageAmount, TSubclassOf<UGameplayEffect> DamageEffectClass,
                                    AActor* DamageSource, const FHitResult& HitResult, bool bMeleeDamage = false);

    // -------------------------------------------------------------------------
    // 敌我判定（公共规则，静态）
    // -------------------------------------------------------------------------
    /**
     * 敌我判定：判断 Source 是否可以伤害 Target。
     *
     * 为什么做成"ASC 上的静态函数"而不是各技能各自实现：
     *  - 这是**战斗公共规则**（谁算敌人），必须只有一份实现，否则规则漂移（技能一份、武器组件一份）；
     *  - 放在 ASC 上是因为它本就是"伤害施加"的收口点（ApplyDamageToTarget* 的兄弟接口），
     *    语义上"伤害入口判定谁能被伤害"最自然；且任何需要战斗判定的组件都已依赖 ASC。
     *
     * 规则：
     *  1) 排除空目标与自伤；
     *  2) 目标必须实现 IAbilitySystemInterface（没有 ASC 的目标不可被 GAS 结算，如场景物件）；
     *  3) 阵营过滤：双方都实现 IGenericTeamAgentInterface 且阵营相同（非 NoTeam）时判为友军，不可伤害。
     */
    UFUNCTION(BlueprintCallable, Category = "Damage")
    static bool IsHostileTarget(AActor* SourceActor, AActor* TargetActor);

    // -------------------------------------------------------------------------
    // 命中 / 击杀确认（服务器 → 攻击者客户端）
    // -------------------------------------------------------------------------
    /**
     * 命中确认（服务器 → **攻击者**客户端，不可靠）。
     *
     * 为什么需要定向（Client）而不是多播：命中标记与命中音效只属于"开枪的人"，
     * 广播给所有人既浪费带宽又会让旁观者误以为是自己打中的。
     * 为什么用不可靠：命中是高频事件（连发武器每发一次），可靠 RPC 会堆积、延迟；
     * 丢一两次命中标记不影响玩法（音效/视觉都是瞬时反馈）。
     *
     * @param HitTarget   被打中的目标
     * @param DamageAmount 本次造成的伤害（原始值；便于 HUD 显示伤害数字）
     * @param bHeadshot   命中部位是否为头部（服务器用受击方的 CombatComponent 判定，规则单一来源）
     */
    UFUNCTION(Client, Unreliable)
    void ClientNotifyHitConfirmed(AActor* HitTarget, float DamageAmount, bool bHeadshot);

    /**
     * 击杀确认（服务器 → **击杀者**客户端，可靠）。
     * 为什么用可靠：击杀是低频且高价值的反馈，"击杀提示"丢一次会让玩家以为没杀到，故不允许丢包。
     */
    UFUNCTION(Client, Reliable)
    void ClientNotifyKillConfirmed(AActor* VictimActor);

    /** 命中确认事件（攻击者本端订阅：命中标记 / 命中音效 / 伤害数字） */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnGameHitConfirmed OnHitConfirmed;

    /** 击杀确认事件（击杀者本端订阅：击杀提示 / 击杀音效 / 连杀计数） */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnGameKillConfirmed OnKillConfirmed;

    // -------------------------------------------------------------------------
    // 技能激活（按输入标签）
    // -------------------------------------------------------------------------
    /**
     * 按"输入标签"激活技能：把所有 AbilityTags 含该标签的技能一并尝试激活。
     *
     * 说明：这是"输入 → 技能"的解耦入口（角色输入层、Lua 规则/表现层都可调用）；
     *       是否本地预测还是上服务器，由各技能自身的 NetExecutionPolicy 决定（引擎内建）。
     *
     * @param InputTag 输入标签（须与技能 AbilityTags 中的标签一致）
     * @return 是否至少有一个技能被成功激活
     */
    UFUNCTION(BlueprintCallable, Category = "Ability")
    bool TryActivateAbilitiesByInputTag(FGameplayTag InputTag);

protected:
    /**
     * 伤害施加的公共实现。
     * @param HitResult 命中细节，可为空（为空表示无命中信息，如爆炸/范围伤害）
     * @param bMeleeDamage 是否近战伤害（为 true 时给规格附加动态资产标签 Damage.Source.Melee）
     */
    bool ApplyDamageInternal(AActor* TargetActor, float DamageAmount, TSubclassOf<UGameplayEffect> DamageEffectClass,
                             AActor* DamageSource, const FHitResult* HitResult, bool bMeleeDamage = false);
};
