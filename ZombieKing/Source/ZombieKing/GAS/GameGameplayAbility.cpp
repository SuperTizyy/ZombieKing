#include "GameGameplayAbility.h"
#include "GameAbilitySystemComponent.h"      // 自定义 ASC：ApplyDamageToTargetWithHit
#include "AbilitySystemInterface.h"          // IAbilitySystemInterface（目标是否可被 GAS 结算）
#include "GenericTeamAgentInterface.h"       // IGenericTeamAgentInterface / FGenericTeamId（阵营过滤）
#include "GameFramework/Pawn.h"              // APawn：GetController / GetActorEyesViewPoint
#include "GameFramework/Controller.h"        // AController::GetPlayerViewPoint

UGameGameplayAbility::UGameGameplayAbility()
{
    // 每个角色一份技能实例：允许技能保存跨帧状态（蓄力/连段/瞄准等）
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

    // 默认仅服务器执行（最保守、最不易出错）：
    // 需要客户端即时反馈的技能，请在其蓝图子类的 Class Defaults 中改为 LocalPredicted。
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

bool UGameGameplayAbility::IsValidAttackTarget(AActor* Target) const
{
    // 规则实现已收敛到 UGameAbilitySystemComponent::IsHostileTarget（静态）：
    // 武器组件等"非技能侧"的战斗代码也要用同一套敌我判定，两处各自实现迟早会规则漂移。
    return UGameAbilitySystemComponent::IsHostileTarget(GetAvatarActorFromActorInfo(), Target);
}

bool UGameGameplayAbility::ApplyDamageToTarget(AActor* Target, float DamageAmount,
                                               TSubclassOf<UGameplayEffect> DamageEffectClass, const FHitResult* HitResult,
                                               bool bMeleeDamage) const
{
    // 统一入口再做一次合法性校验，防止子类绕过（同时保证友伤被拒）
    if (!IsValidAttackTarget(Target))
    {
        return false;
    }

    // 施加方 = 本技能持有者（AvatarActor）所托管的 ASC（本项目 ASC 位于 PlayerState）
    UGameAbilitySystemComponent* SourceASC = Cast<UGameAbilitySystemComponent>(GetAbilitySystemComponentFromActorInfo());
    if (!SourceASC)
    {
        return false;
    }

    // 无命中信息时传一个空 FHitResult：接口按"无命中细节"处理（受击方将退化为躯干判定）
    const FHitResult EmptyHitResult;
    return SourceASC->ApplyDamageToTargetWithHit(
        Target, DamageAmount, DamageEffectClass, GetAvatarActorFromActorInfo(),
        HitResult ? *HitResult : EmptyHitResult, bMeleeDamage);
}

void UGameGameplayAbility::GetAttackAimViewPoint(FVector& OutLocation, FRotator& OutRotation) const
{
    // 优先走"控制器视角"：这是玩家准星所指，也是服务器上唯一能拿到的、与客户端显示一致的瞄准方向
    // （服务器端由 APlayerController::ServerUpdateCamera 写入客户端上报的相机 POV）。
    if (const APawn* AvatarPawn = Cast<APawn>(GetAvatarActorFromActorInfo()))
    {
        if (const AController* OwningController = AvatarPawn->GetController())
        {
            OwningController->GetPlayerViewPoint(OutLocation, OutRotation);
            return;
        }

        // 无控制器（AI 未附身 / 单元测试）：退回角色眼睛位置与朝向
        AvatarPawn->GetActorEyesViewPoint(OutLocation, OutRotation);
        return;
    }

    // 非 Pawn 的 Avatar（异常兜底）：用 Actor 自身位置与朝向，保证不崩溃
    const AActor* AvatarActor = GetAvatarActorFromActorInfo();
    OutLocation = AvatarActor ? AvatarActor->GetActorLocation() : FVector::ZeroVector;
    OutRotation = AvatarActor ? AvatarActor->GetActorRotation() : FRotator::ZeroRotator;
}
