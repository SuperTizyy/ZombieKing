#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GameGameplayAbility.generated.h"

class UGameplayEffect;
struct FHitResult;

/**
 * 竞技生化网游：技能基类（所有角色技能 / 生化幽灵专属技能的 C++ 父类）
 *
 * 职责与边界：
 *  - 本类只设定"竞技网游的技能默认策略"，不含任何具体技能逻辑；
 *  - 具体技能的数值、表现与流程应做成蓝图子类（内容资产）——改技能无需重新编译 C++（满足热更要求）；
 *  - 需要跨帧状态、需要被服务器权威裁决的技能，其执行位置与实例化策略由下方默认值统一约定。
 *
 * 为什么需要这个 C++ 基类：
 *  - 统一 InstancingPolicy / NetExecutionPolicy 的默认值，避免每个技能各自配置导致行为不一致；
 *  - 为后续公共能力（如技能冷却、输入绑定、Lua 表现派发）预留统一落点。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UGameGameplayAbility : public UGameplayAbility
{
    GENERATED_BODY()

public:
    UGameGameplayAbility();

    // -------------------------------------------------------------------------
    // 说明（供子类参考）：以下为基类设定的默认策略，蓝图子类可在 Class Defaults → Advanced 中按需覆盖
    // -------------------------------------------------------------------------
    //  - InstancingPolicy = InstancedPerActor：
    //      每个角色一份技能实例，技能内部可安全保存跨帧状态（如蓄力、连段、瞄准），
    //      是多数持久型战斗技能的默认选择；
    //  - NetExecutionPolicy = ServerOnly：
    //      默认只在服务器执行，最保守、最不易出错；
    //      若某技能需要"客户端即时反馈 + 服务器裁决"（如射击、位移），
    //      该技能蓝图应自行改为 LocalPredicted，并确保其逻辑可被预测与回滚。

    // -------------------------------------------------------------------------
    // 公共战斗助手（供"攻击类"技能子类复用：阵营过滤 / 伤害施加 / 瞄准取点）
    // 说明：这些是"不可热更的机制"（阵营语义、GAS 归因链路），故放在 C++；
    //       而伤害数值、攻击范围、散布等"可调数值"一律放在各技能的蓝图资产默认值里。
    // -------------------------------------------------------------------------

    /**
     * 判断目标是否为本技能可伤害的敌对目标。
     * 过滤规则：
     *  1) 排除空指针与自身；
     *  2) 目标必须实现 IAbilitySystemInterface（没有 ASC 的目标不可被 GAS 结算，如场景物件）；
     *  3) 阵营过滤：当"自身"与"目标"双方都实现 IGenericTeamAgentInterface 且阵营相同（非 NoTeam）时判为友军，不可伤害。
     * @param Target 待判定的目标 Actor
     * @return 是否可被本技能伤害
     */
    bool IsValidAttackTarget(AActor* Target) const;

    /**
     * 对目标施加一次伤害（服务器权威；内部会再走一次 IsValidAttackTarget 校验）。
     * 链路：本技能持有者（AvatarActor）的 ASC → ApplyDamageToTargetWithHit（SetByCaller 写入伤害值，
     *       并由伤害 GE 引用的 UGameDamageExecution 在服务器结算）。
     * @param Target            受击目标
     * @param DamageAmount      本次原始伤害值（正数）
     * @param DamageEffectClass 伤害效果类（内容资产，需引用 UGameDamageExecution）
     * @param HitResult         命中细节（可为空：如范围伤害；携带后受击方可做部位判定与受击表现）
     * @param bMeleeDamage      是否为近战伤害（近战技能置 true → 受击方可做"刀杀禁令"判定）
     * @return 是否成功施加
     */
    bool ApplyDamageToTarget(AActor* Target, float DamageAmount, TSubclassOf<UGameplayEffect> DamageEffectClass, const FHitResult* HitResult, bool bMeleeDamage = false) const;

    /**
     * 取"本次攻击的瞄准起点与方向"。
     * 约定：优先使用控制器的玩家视角（GetPlayerViewPoint）——在服务器上即为客户端上报的相机 POV
     *       （见 APlayerController::ServerUpdateCamera：服务器默认不重算相机，保留客户端上报值），
     *       因此"开枪/挥爪"方向与玩家看到的准星一致；无控制器的 AI/测试场景回退到角色眼睛位置与朝向。
     * @param OutLocation 瞄准起点（相机/眼睛位置）
     * @param OutRotation 瞄准方向
     */
    void GetAttackAimViewPoint(FVector& OutLocation, FRotator& OutRotation) const;
};
