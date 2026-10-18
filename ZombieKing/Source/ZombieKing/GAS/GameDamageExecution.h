#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"   // UGameplayEffectExecutionCalculation 基类与属性捕获宏
#include "GameDamageExecution.generated.h"

/**
 * 竞技生化网游：伤害执行计算（服务器权威结算）
 *
 * 定位：
 *  - "伤害 GE 资产"引用本执行计算，从而把"伤害如何结算"的公式固定下来；
 *  - 每次伤害的具体数值不写死在资产里，而由施加方通过 SetByCaller(Data.Damage) 传入
 *    （见 UGameAbilitySystemComponent::ApplyDamageToTarget），因此一份伤害 GE 服务所有伤害来源；
 *  - 伤害一律在服务器结算：执行计算只在服务器运行，客户端不会据此改数值（防作弊的根基）。
 *
 * 与热更新原则的关系：
 *  - "公式"（读取原始伤害 → 扣生命）属于结构性逻辑，放在 C++；
 *  - "数值"（每次打多少）由 Lua/技能在运行期传入，可随时调整，无需重编译。
 *
 * TODO（后续里程碑，按需再补，当前刻意不做以免过度设计）：
 *  - 伤害类型/部位倍率（爆头、弱点）→ 可在本计算中读取额外 SetByCaller 或标签后乘以倍率；
 *  - 距离衰减、护甲减伤（当前项目无护甲）。
 */
UCLASS()
class ZOMBIEKING_API UGameDamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()

public:
    UGameDamageExecution();

    /**
     * 执行伤害结算。
     * 流程：读取 SetByCaller 原始伤害 → 读取目标当前生命 → 钳制（不把生命扣成负数）
     *      → 以负向加法修饰符提交给 GAS 统一结算。
     *
     * @param ExecutionParams     本次执行参数（含效果规格与属性捕获入口）
     * @param OutExecutionOutput  [OUT] 输出修饰符，由 GAS 汇总后写回属性
     */
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
                                        FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
