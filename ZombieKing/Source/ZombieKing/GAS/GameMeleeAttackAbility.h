#pragma once

#include "CoreMinimal.h"
#include "GameGameplayAbility.h"
#include "GameMeleeAttackAbility.generated.h"

/**
 * 竞技生化网游：近战攻击技能基类（生化幽灵爪击等）
 *
 * 执行模型（ServerOnly 服务器权威）：
 *  - 客户端按下攻击键 → 按输入标签请求激活 → 服务器执行本技能；
 *  - 服务器以"瞄准起点（相机/眼睛）沿准星方向"做一次球体扫掠，命中范围内的敌对目标逐个结算伤害；
 *  - 客户端即时反馈（挥击蒙太奇/音效）由角色 Lua 在输入回调中先行播放（见 GameCharacterBase.lua 的 OnInputAttack），
 *    不参与权威判定，符合"表现下沉、判定上行"的分层原则。
 *
 * 热更新边界：
 *  - 伤害值、扫掠距离/半径、命中通道等"可调数值"全部为本类默认值（EditDefaultsOnly），
 *    在技能蓝图子类中调整即可，无需重新编译 C++；
 *  - 阵营过滤、GAS 伤害归因链路等"不可热更的机制"在 C++ 父类中（见 UGameGameplayAbility）。
 *
 * 内容侧使用：
 *  1) 用本类做父类建技能蓝图（如 GA_ZombieClaw），设置 DamageEffectClass（引用 UGameDamageExecution 的伤害 GE）；
 *  2) 在该蓝图 AbilityTags 中加入输入标签（如 Ability.Input.Attack）；
 *  3) 把该技能挂进对应角色的 UGameCharacterDefinition::AbilitySets。
 */
UCLASS()
class ZOMBIEKING_API UGameMeleeAttackAbility : public UGameGameplayAbility
{
    GENERATED_BODY()

public:
    UGameMeleeAttackAbility();

protected:
    /** 技能激活：服务器上执行一次球体扫掠并结算伤害，随后结束技能 */
    virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
                                 const FGameplayAbilityActivationInfo ActivationInfo,
                                 const FGameplayEventData* TriggerEventData) override;

    // -------------------------------------------------------------------------
    // 数值配置（内容侧在技能蓝图子类中调整）
    // -------------------------------------------------------------------------
    /** 单次爪击的原始伤害值（正数；实际扣血由伤害 GE + UGameDamageExecution 在服务器结算） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Damage", meta = (ClampMin = "0.0"))
    float MeleeDamage = 35.0f;

    /** 伤害效果类：需引用 UGameDamageExecution 作为执行计算（每种攻击可复用同一份 GE 资产） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Damage")
    TSubclassOf<UGameplayEffect> DamageEffectClass;

    /** 攻击距离（cm）：从瞄准起点沿准星向前的最大扫掠长度 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Trace", meta = (ClampMin = "0.0"))
    float MeleeRange = 200.0f;

    /**
     * 扫掠球半径（cm）：决定爪击判定的"粗细"。
     * 体型越大的生化幽灵应配置更大的半径（配合体型差异化设计）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Trace", meta = (ClampMin = "0.0"))
    float MeleeRadius = 60.0f;

    /**
     * 追踪通道：默认 ECC_Pawn。
     * 原因（引擎事实）：角色胶囊体默认使用 "Pawn" 碰撞配置，该配置显式忽略了 Visibility 通道
     * （BaseEngine.ini：Pawn 配置 CustomResponses 中 Visibility=ECR_Ignore），
     * 因此若用 Visibility 做通道将扫不到角色；ECC_Pawn 同时能被墙体（BlockAll）阻挡。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Trace")
    TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Pawn;

    /** 是否绘制调试扫掠（仅开发期使用；打包构建中 DrawDebug 会被自动剔除） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee|Debug")
    bool bDrawDebug = false;
};
