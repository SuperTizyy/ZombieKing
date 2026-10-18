#include "GameDamageExecution.h"
#include "GameAttributeSet.h"      // UGameAttributeSet::GetHealthAttribute()（输出修饰符的目标属性）
#include "GameEffectTags.h"        // GameEffectTags::Data_Damage（SetByCaller 原始伤害）

// =============================================================================
//  属性捕获定义
//  GAS 的既定机制：执行计算只能读取"被显式捕获"的属性；
//  这里捕获目标(Target)的 Health，用于把伤害钳制在"不超过当前生命"。
// =============================================================================
namespace GameDamageExecutionPrivate
{
    struct FGameDamageStatics
    {
        /** 生成 HealthProperty（FProperty*）与 HealthDef（FGameplayEffectAttributeCaptureDefinition） */
        DECLARE_ATTRIBUTE_CAPTUREDEF(Health);

        FGameDamageStatics()
        {
            // 捕获目标的 Health：
            //  - 来源 = Target（被击中者）；
            //  - bSnapshot = false（读取实时值，而非效果施加瞬间的快照）。
            DEFINE_ATTRIBUTE_CAPTUREDEF(UGameAttributeSet, Health, Target, false);
        }
    };

    /** 单例访问：捕获定义在首次使用时构造，之后复用 */
    static const FGameDamageStatics& DamageStatics()
    {
        static FGameDamageStatics Statics;
        return Statics;
    }
}

UGameDamageExecution::UGameDamageExecution()
{
    // 声明本次执行必须捕获的属性（目标生命）：
    // 未在构造期登记，AttemptCalculateCapturedAttributeMagnitude 将读取失败（恒为 0）。
    RelevantAttributesToCapture.Add(GameDamageExecutionPrivate::DamageStatics().HealthDef);
}

void UGameDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                                 FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
    // -------------------------------------------------------------------------
    // 步骤 1：读取"原始伤害"（由施加方通过 SetByCaller 写入，WarnIfNotFound=false 表示缺失时静默取默认值）
    // -------------------------------------------------------------------------
    const FGameplayEffectSpec& OwningSpec = ExecutionParams.GetOwningSpec();
    const float RawDamage = OwningSpec.GetSetByCallerMagnitude(GameEffectTags::Data_Damage, /*WarnIfNotFound=*/false, /*DefaultIfNotFound=*/0.0f);

    // 无效伤害（0 或负）直接跳过：避免负伤害被反向解释成治疗
    if (RawDamage <= 0.0f)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 步骤 2：读取被击中目标的当前生命（经由捕获的属性聚合器）
    // -------------------------------------------------------------------------
    FAggregatorEvaluateParameters EvaluationParameters;
    float TargetHealth = 0.0f;
    ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(
        GameDamageExecutionPrivate::DamageStatics().HealthDef, EvaluationParameters, TargetHealth);

    // -------------------------------------------------------------------------
    // 步骤 3：把伤害钳制为"不超过当前生命"，避免出现负血量
    //（AttributeSet 的 PostGameplayEffectExecute 还会再钳制一次，这里是第一道防线）
    // -------------------------------------------------------------------------
    const float DamageToApply = FMath::Min(RawDamage, FMath::Max(TargetHealth, 0.0f));
    if (DamageToApply <= 0.0f)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 步骤 4：以"负向加法修饰符"提交生命扣减，交由 GAS 统一汇总写回属性
    // 说明：GAS 会随后触发属性变化事件 → HealthComponent 收事件 → 广播给 Lua 做表现
    // -------------------------------------------------------------------------
    OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(
        UGameAttributeSet::GetHealthAttribute(),
        EGameplayModOp::Additive,
        -DamageToApply));
}
