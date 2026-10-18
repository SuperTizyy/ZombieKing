#include "CombatComponent.h"
#include "AbilitySystemComponent.h"
#include "GameAttributeSet.h"              // UGameAttributeSet::GetHealthAttribute()
#include "GameEffectTags.h"                // GameEffectTags::Damage_Source_Melee（近战命中判定）
#include "GameplayEffect.h"                // FGameplayEffectSpec（读取命中上下文 / GetAllAssetTags）
#include "GameplayEffectExtension.h"       // FGameplayEffectModCallbackData 完整定义

UCombatComponent::UCombatComponent()
{
    // 纯逻辑组件：由受击事件/委托驱动，不启用 Tick
    PrimaryComponentTick.bCanEverTick = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UCombatComponent::InitializeWithAbilitySystem(UAbilitySystemComponent* InASC)
{
    if (!InASC)
    {
        return;
    }

    // 先解绑旧订阅：重生/重连可能触发本函数，不去重会导致同一次受击被多次转发
    UnbindAbilitySystemDelegates();

    AbilitySystemComponent = InASC;

    // 订阅生命属性变化：
    //  - 使用非动态多播委托 AddUObject（GAS 的属性变化委托不是 UPROPERTY 动态委托）；
    //  - 句柄必须保存，用于 EndPlay 解绑（否则组件销毁后回调悬空）。
    HealthChangedDelegateHandle = InASC
        ->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetHealthAttribute())
        .AddUObject(this, &UCombatComponent::HandleHealthAttributeChanged);
}

void UCombatComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 组件销毁前解绑委托，防止"角色销毁后 ASC 回调悬空"崩溃
    UnbindAbilitySystemDelegates();
    Super::EndPlay(EndPlayReason);
}

void UCombatComponent::UnbindAbilitySystemDelegates()
{
    if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
    {
        if (HealthChangedDelegateHandle.IsValid())
        {
            ASC->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetHealthAttribute())
               .Remove(HealthChangedDelegateHandle);
        }
    }
    HealthChangedDelegateHandle.Reset();
}

// =============================================================================
//  受击处理
// =============================================================================

void UCombatComponent::HandleHealthAttributeChanged(const FOnAttributeChangeData& Data)
{
    // 只处理"受伤"：Delta < 0 才算受击（回血 / 生命上限变化不触发受击流程）
    const float Delta = Data.NewValue - Data.OldValue;
    if (Delta >= 0.0f)
    {
        return;
    }

    // 受击伤害取正数，便于表现层直接使用
    const float DamageAmount = -Delta;

    // -------------------------------------------------------------------------
    // 从伤害效果上下文中取出命中细节（部位骨骼、攻击来源）
    // 这条通道是"服务器权威 + 客户端表现"之间的标准做法：
    // 伤害由服务器结算，但命中细节随 GameplayEffect 上下文一同送达，受击方据此做部位判定与表现。
    // -------------------------------------------------------------------------
    FName HitBoneName = NAME_None;
    AActor* InstigatorActor = nullptr;
    bool bMeleeHit = false;

    if (Data.GEModData)
    {
        const FGameplayEffectContextHandle& EffectContext = Data.GEModData->EffectSpec.GetContext();
        if (const FHitResult* HitResult = EffectContext.GetHitResult())
        {
            HitBoneName = HitResult->BoneName;
        }
        InstigatorActor = EffectContext.GetInstigator();

        // 近战命中判定：读取伤害规格的"资产标签"（动态标签 + GE 定义资产标签的合集，引擎事实），
        // 近战链路施加时附加了 Damage.Source.Melee（见 UGameAbilitySystemComponent::ApplyDamageInternal）。
        // 说明：属性变化回调在客户端也会触发，但"动态资产标签是否随复制到达客户端"不作为判定依据 ——
        //       本结论只在服务器侧有意义（Lua 死亡规则读的是服务器缓存），客户端那份恒为 false 也无害。
        FGameplayTagContainer SpecAssetTags;
        Data.GEModData->EffectSpec.GetAllAssetTags(SpecAssetTags);
        bMeleeHit = SpecAssetTags.HasTagExact(GameEffectTags::Damage_Source_Melee);
    }

    // 部位判定
    const EHitZone HitZone = DetermineHitZone(HitBoneName);

    // 受击硬直：服务器计算时长并广播（表现层由 Lua 订阅 OnStaggered 播放硬直动画/音效）。
    // TODO（后续里程碑）：移动层面的硬直（短暂限制移动/打断爬墙吸附）需要在 Mover 运动模式上做，
    //                     属于"移动效果"专项，故此处只提供时长与事件，不改动移动数值。
    const float StaggerDuration = GetStaggerDuration(HitZone);
    if (StaggerDuration > 0.0f)
    {
        OnStaggered.Broadcast(StaggerDuration, HitZone, InstigatorActor);
    }

    // 转发受击事件：由角色（AGameCharacterBase）订阅后派发给 Lua 受击表现；
    // 近战结论一并携带（角色缓存"最近一次受击是否近战"，供刀杀禁令判定）
    OnDamagedReceived.Broadcast(DamageAmount, HitBoneName, InstigatorActor, bMeleeHit);
}

// =============================================================================
//  部位判定
// =============================================================================

EHitZone UCombatComponent::DetermineHitZone(FName HitBoneName) const
{
    // 无骨骼信息（如爆炸/范围伤害）按躯干处理
    if (HitBoneName.IsNone())
    {
        return EHitZone::Torso;
    }

    // 1) 精确查表优先：不同骨架的骨骼命名不同（人类 vs 多足生物），由各角色的蓝图子类配置映射
    if (const EHitZone* ConfiguredZone = BoneToHitZone.Find(HitBoneName))
    {
        return *ConfiguredZone;
    }

    // 2) 兜底规则：骨骼名含 "head" 视为头部（人类与多数生物骨架的通用命名）
    if (HitBoneName.ToString().Contains(TEXT("head")))
    {
        return EHitZone::Head;
    }

    // 3) 其余一律视为躯干
    return EHitZone::Torso;
}

float UCombatComponent::GetHitZoneDamageMultiplier(EHitZone HitZone) const
{
    // 配置优先：各角色蓝图可覆写（例如生化幽灵弱点部位特殊处理）
    if (const float* Configured = HitZoneDamageMultipliers.Find(HitZone))
    {
        return FMath::Max(*Configured, 0.0f);
    }

    // 引擎默认倍率（未配置时的兜底）
    switch (HitZone)
    {
    case EHitZone::Head:  return 2.0f;
    case EHitZone::Limb:  return 0.75f;
    case EHitZone::Torso:
    default:              return 1.0f;
    }
}

float UCombatComponent::GetStaggerDuration(EHitZone HitZone) const
{
    // 未配置该部位硬直则视为"不硬直"，而不是给一个臆造默认值
    if (const float* Configured = HitZoneStaggerDurations.Find(HitZone))
    {
        return FMath::Max(*Configured, 0.0f);
    }
    return 0.0f;
}

// =============================================================================
//  反作弊：命中合理性校验
// =============================================================================

FHitValidationResult UCombatComponent::ValidateIncomingHit(const FHitResult& HitResult, AActor* InstigatorActor) const
{
    FHitValidationResult Result;

    // 1) 来源必须存在：无法归因的命中视为可疑
    if (!InstigatorActor)
    {
        Result.bValid = false;
        Result.Reason = EHitRejectionReason::NoInstigator;
        return Result;
    }

    // 2) 距离校验：命中点到攻击来源的距离不得超过允许射程
    const FVector InstigatorLocation = InstigatorActor->GetActorLocation();
    Result.MeasuredDistance = (HitResult.ImpactPoint - InstigatorLocation).Size();
    if (Result.MeasuredDistance > MaxHitValidationRange)
    {
        Result.bValid = false;
        Result.Reason = EHitRejectionReason::OutOfRange;
        return Result;
    }

    // 3) 角度校验：攻击来源"看向命中点"的方向必须与其当前视线方向在允许偏差内。
    //    这是防"隔着墙/背身命中"这类明显不合理上报的最低成本手段。
    if (const APawn* InstigatorPawn = Cast<APawn>(InstigatorActor))
    {
        const FVector ViewDirection = InstigatorPawn->GetControlRotation().Vector();
        const FVector ToHitDirection =
            (HitResult.ImpactPoint - InstigatorPawn->GetPawnViewLocation()).GetSafeNormal();

        const float DotValue = FVector::DotProduct(ViewDirection, ToHitDirection);
        const float AngleDegrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(DotValue, -1.0f, 1.0f)));

        if (AngleDegrees > MaxHitValidationAngleDeviation)
        {
            Result.bValid = false;
            Result.Reason = EHitRejectionReason::AngleViolation;
            return Result;
        }
    }

    // 全部通过
    Result.bValid = true;
    Result.Reason = EHitRejectionReason::None;
    return Result;
}

void UCombatComponent::BroadcastHitValidationResult(const FHitValidationResult& Result)
{
    // 回执广播：把校验结论告知相关端（例如被拒绝的攻击者），由其决定回滚预测表现或记录违规
    OnHitValidationResult.Broadcast(Result);
}
