#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"      // FOnAttributeChangeData（订阅 ASC 属性变化需要完整类型）
#include "CombatComponent.generated.h"

class UAbilitySystemComponent;
struct FOnAttributeChangeData;

/** 命中部位（决定伤害倍率与硬直时长） */
UENUM(BlueprintType)
enum class EHitZone : uint8
{
    /** 躯干：基准伤害区 */
    Torso UMETA(DisplayName = "Torso (躯干)"),

    /** 头部：高倍率、强硬直 */
    Head UMETA(DisplayName = "Head (头部)"),

    /** 四肢：低倍率 */
    Limb UMETA(DisplayName = "Limb (四肢)"),

    /** 枚举哨兵：用于数组容量与越界校验，绝不作为真实部位使用 */
    MAX UMETA(Hidden)
};

/** 反作弊命中校验的拒绝原因 */
UENUM(BlueprintType)
enum class EHitRejectionReason : uint8
{
    /** 通过 */
    None UMETA(DisplayName = "None (通过)"),

    /** 命中点超出允许射程 */
    OutOfRange UMETA(DisplayName = "OutOfRange (超出射程)"),

    /** 命中方向与攻击者视线偏差过大 */
    AngleViolation UMETA(DisplayName = "AngleViolation (角度不符)"),

    /** 缺少攻击来源（无法归因，视为可疑） */
    NoInstigator UMETA(DisplayName = "NoInstigator (无来源)"),

    /** 枚举哨兵 */
    MAX UMETA(Hidden)
};

/** 命中校验结果（反作弊回执） */
USTRUCT(BlueprintType)
struct FHitValidationResult
{
    GENERATED_BODY()

    /** 是否通过校验 */
    UPROPERTY(BlueprintReadOnly, Category = "Hit")
    bool bValid = false;

    /** 未通过时的拒绝原因 */
    UPROPERTY(BlueprintReadOnly, Category = "Hit")
    EHitRejectionReason Reason = EHitRejectionReason::None;

    /** 实测距离（cm），便于日志排查与回执展示 */
    UPROPERTY(BlueprintReadOnly, Category = "Hit")
    float MeasuredDistance = 0.0f;
};

/**
 * 受击事件广播（转发给角色 → Lua 表现层）。
 * bMeleeHit：本次伤害是否近战（刀/爪）命中 —— 服务器侧由近战链路在伤害规格上
 * 附加动态资产标签 Damage.Source.Melee，本组件在属性回调里读出；
 * 角色据此缓存"最近一次受击是否近战"，供 Lua 死亡规则实现 CF 式"刀杀禁令"。
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnDamagedReceivedSignature, float, DamageAmount, FName, HitBone, AActor*, InstigatorActor, bool, bMeleeHit);

/** 受击硬直广播（表现层据此播放硬直动画/音效） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnStaggeredSignature, float, Duration, EHitZone, HitZone, AActor*, InstigatorActor);

/** 命中校验回执广播 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHitValidationResultSignature, const FHitValidationResult&, Result);

/**
 * 竞技生化网游：战斗逻辑组件（部位判定 / 受击硬直 / 反作弊校验）
 *
 * 架构定位（严格遵循热更新原则）：
 *  - 本组件只做"战斗辅助逻辑"的**机制**：部位判定、倍率/硬直查表、命中合理性校验；
 *  - 权威伤害数值仍由 GAS GameplayEffect 在服务器结算（伤害进 UGameDamageExecution），本组件不参与算血；
 *  - 所有可调数值（各部位倍率、硬直时长、校验射程/角度）均为 EditDefaultsOnly 配置，
 *    改数值只需改蓝图子类默认值，无需重编译 C++；
 *  - 具体表现（硬直动画、受击演出）由 Lua 订阅下方事件后处理。
 *
 * 命中信息（部位/来源）的来源：
 *  - 伤害 GE 的效果上下文携带 FHitResult（见 UGameAbilitySystemComponent::ApplyDamageToTargetWithHit）；
 *  - 本组件从 ASC 的属性变化回调里取出该命中信息，从而做部位判定与硬直。
 */
UCLASS()
class ZOMBIEKING_API UCombatComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UCombatComponent();

    /**
     * 与 GAS 建立订阅（在玩家状态 ASC 就绪后由角色基类调用）。
     * 可重复调用（重生/重连）：内部会先解绑旧订阅，避免事件重复转发。
     */
    void InitializeWithAbilitySystem(UAbilitySystemComponent* InASC);

    // -------------------------------------------------------------------------
    // 部位判定
    // -------------------------------------------------------------------------
    /** 依据命中骨骼名判定命中部位（查表；未配置的骨骼按"骨骼名是否含 head 关键字"兜底，再退化为躯干） */
    UFUNCTION(BlueprintPure, Category = "Combat|HitZone")
    EHitZone DetermineHitZone(FName HitBoneName) const;

    /** 取该部位的伤害倍率（查表；未配置时按引擎默认 头部2.0 / 躯干1.0 / 四肢0.75） */
    UFUNCTION(BlueprintPure, Category = "Combat|HitZone")
    float GetHitZoneDamageMultiplier(EHitZone HitZone) const;

    /** 取该部位的受击硬直时长（秒，查表；未配置时返回 0） */
    UFUNCTION(BlueprintPure, Category = "Combat|Stagger")
    float GetStaggerDuration(EHitZone HitZone) const;

    // -------------------------------------------------------------------------
    // 反作弊：命中合理性校验（服务器复算）
    // -------------------------------------------------------------------------
    /**
     * 校验一次命中是否合理（服务器权威复算）。
     * 校验项：
     *  1) 攻击来源存在（缺失视为可疑）；
     *  2) 命中点到来源的距离不超过 MaxHitValidationRange；
     *  3) 若来源是 Pawn，则"来源视线方向"与"来源→命中点方向"的夹角不超过 MaxHitValidationAngleDeviation。
     * 说明：本函数只做"物理合理性"判定，不做业务裁决；拒绝后的处置（回滚/惩罚）由服务器规则决定。
     */
    UFUNCTION(BlueprintPure, Category = "Combat|Validation")
    FHitValidationResult ValidateIncomingHit(const FHitResult& HitResult, AActor* InstigatorActor) const;

    // -------------------------------------------------------------------------
    // 对外事件（C++ / 蓝图 / UnLua 均可订阅）
    // -------------------------------------------------------------------------
    /** 受击事件：角色收到后转发给 Lua 受击表现（DamageAmount 恒为正数） */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnDamagedReceivedSignature OnDamagedReceived;

    /** 受击硬直事件 */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnStaggeredSignature OnStaggered;

    /** 命中校验回执事件（由调用方在拿到 ValidateIncomingHit 结果后自行广播，或订阅此处） */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnHitValidationResultSignature OnHitValidationResult;

    /** 广播一次命中校验回执（供调用方在拒绝命中时告知相关端） */
    UFUNCTION(BlueprintCallable, Category = "Combat|Validation")
    void BroadcastHitValidationResult(const FHitValidationResult& Result);

protected:
    /** 组件销毁时解绑 ASC 委托，防止回调悬空 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** ASC 的 Health 属性变化回调：只处理"受伤"（Delta < 0），驱动部位判定 → 硬直 → 受击事件 */
    void HandleHealthAttributeChanged(const FOnAttributeChangeData& Data);

    /** 解绑全部 ASC 订阅（重复初始化与 EndPlay 共用） */
    void UnbindAbilitySystemDelegates();

protected:
    /** 部位 → 伤害倍率（可在角色蓝图子类中按怪物类型覆写；未配置用引擎默认值） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|HitZone")
    TMap<EHitZone, float> HitZoneDamageMultipliers;

    /**
     * 骨骼名 → 命中部位 映射。
     * 说明：不同骨架的骨骼命名不同（人类 vs 多足生物），故映射必须由各角色的蓝图子类配置，
     *       未配置时会退化为"骨骼名含 head 视为头部、其余视为躯干"的兜底规则。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|HitZone")
    TMap<FName, EHitZone> BoneToHitZone;

    /** 部位 → 受击硬直时长（秒；未配置视为无硬直） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Stagger")
    TMap<EHitZone, float> HitZoneStaggerDurations;

    /** 命中校验允许的最大距离（cm）：超出即判为不合理 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Validation", meta = (ClampMin = "0"))
    float MaxHitValidationRange = 12000.0f;

    /** 命中校验允许的最大视线偏差（度）：超出即判为不合理（预留网络时延余量） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat|Validation", meta = (ClampMin = "0", ClampMax = "180"))
    float MaxHitValidationAngleDeviation = 60.0f;

    /** 缓存的 ASC（弱引用，避免生命周期悬空） */
    TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

    /** 属性变化委托句柄：EndPlay 时据此解绑 */
    FDelegateHandle HealthChangedDelegateHandle;
};
