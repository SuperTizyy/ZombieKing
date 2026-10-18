#pragma once

#include "CoreMinimal.h"
#include "GameGameplayAbility.h"
#include "GameRangedAttackAbility.generated.h"

/**
 * 竞技生化网游：远程攻击技能（开火入口）
 *
 * 职责定位（2026 武器系统里程碑重构后的分工）：
 *  - 本技能是"**开火的入口闸门**"：输入标签 → GAS 激活 → 委托武器组件执行开火；
 *  - 真正的射击机制（射速节流 / 弹药 / 命中结算 / 投射物）全部在 UGameWeaponComponent（服务器权威）；
 *  - 全部数值（伤害 / 射程 / 散布 / 弹匣 / 换弹时长…）在 UGameWeaponDefinition 资产里。
 *
 * 为什么"用技能做入口"而不是让输入直接调武器组件：
 *  - 复用 GAS 的技能阻断能力：被定身/眩晕/缴械（通过 GameplayTag 阻断技能）时自然无法开火；
 *  - 复用技能冷却与消耗（CommitAbility）：若某把武器需要"消耗能量才能开火"，无需另建机制；
 *  - 与项目既有输入管线一致（输入动作 → 技能标签 → 按标签激活）。
 *
 * 执行模型：
 *  - NetExecutionPolicy = ServerOnly（继承自 UGameGameplayAbility 默认值）：
 *    开火判定与伤害只在服务器进行；
 *  - 客户端即时反馈（枪口火焰/后坐力/音效）由 Lua 在输入回调中先行播放，不参与权威判定
 *    （见 Content/Script/Lua/Core/Character/GameCharacterBase.lua 的 OnInputAbility 处理）；
 *  - 服务器对"别人在开枪"的表现广播由武器组件的不可靠多播负责。
 *
 * 内容侧使用：
 *  1) 用本类做父类建技能蓝图（如 GA_WeaponFire），**无需配置任何数值**（数值都在武器定义里）；
 *  2) 在该蓝图 AbilityTags 中加入开火输入标签（GameAbilityTags::Input_Fire）；
 *  3) 把该技能挂进人类角色的 UGameCharacterDefinition::AbilitySets，并把开火键映射到该标签
 *     （见 UGameInputConfig）；
 *  4) 武器的射速/伤害等一切调参都在 UGameWeaponDefinition 资产上完成 —— 改枪感不必重编译 C++。
 */
UCLASS()
class ZOMBIEKING_API UGameRangedAttackAbility : public UGameGameplayAbility
{
    GENERATED_BODY()

public:
    UGameRangedAttackAbility();

protected:
    /** 技能激活：取瞄准 POV 并委托武器组件开火一次，然后结束技能（无论是否真的开火） */
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;
};
