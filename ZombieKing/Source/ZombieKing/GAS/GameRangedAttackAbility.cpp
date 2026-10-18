#include "GameRangedAttackAbility.h"
#include "GameWeaponComponent.h"   // 武器组件：开火机制与状态的唯一权威（本技能只委托它）
#include "GameFramework/Actor.h"   // AActor::FindComponentByClass（按类型取武器组件）
#include "ZombieKing.h"            // LogZombieKing（装配异常告警）

UGameRangedAttackAbility::UGameRangedAttackAbility()
{
    // 开火判定与伤害只在服务器进行：
    // 客户端按下开火键 → 请求激活本技能 → 服务器执行；
    // 客户端侧的"枪口火焰/后坐力/音效"由 Lua 在输入回调里先行播放（不参与权威判定）。
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

void UGameRangedAttackAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                               const FGameplayAbilityActorInfo* ActorInfo,
                                               const FGameplayAbilityActivationInfo ActivationInfo,
                                               const FGameplayEventData* TriggerEventData)
{
    // -------------------------------------------------------------------------
    // 双保险：客户端不得裁定开火与伤害
    // -------------------------------------------------------------------------
    if (!K2_HasAuthority())
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
        return;
    }

    // 提交技能：应用消耗与冷却（未配置则为空操作）。
    // 注意：武器射速由武器组件节流，与本技能的冷却互不冲突 —— 技能冷却用于"更粗粒度"的限制
    //      （例如"每 3 秒只能点射一次"的战术武器），而射速是"每把枪的连续开火节奏"。
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
        return;
    }

    AActor* AvatarActor = GetAvatarActorFromActorInfo();
    if (!AvatarActor)
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
        return;
    }

    // -------------------------------------------------------------------------
    // 取武器组件：开火机制全在它里面（本技能刻意不含任何射击代码）
    // -------------------------------------------------------------------------
    UGameWeaponComponent* WeaponComponent = AvatarActor->FindComponentByClass<UGameWeaponComponent>();
    if (!WeaponComponent)
    {
        // 角色没挂武器组件属于装配错误（人类角色应当有枪）：告警便于定位"按键开火没反应"
        UE_LOG(LogZombieKing, Warning,
            TEXT("[远程攻击技能] 角色 %s 未挂载 UGameWeaponComponent，开火请求被忽略"), *AvatarActor->GetName());
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
        return;
    }

    // -------------------------------------------------------------------------
    // 取瞄准 POV 并委托开火
    // -------------------------------------------------------------------------
    // 服务器上的 POV 即客户端上报的相机位置与朝向（引擎由 APlayerController::ServerUpdateCamera 写入，
    // 且服务器默认不重算相机），因此"打得准不准"完全由服务器复算，客户端无法凭伪造命中作弊。
    FVector AimLocation;
    FRotator AimRotation;
    GetAttackAimViewPoint(AimLocation, AimRotation);

    // 返回值表示"本次是否真的开火"（可能因换弹中 / 弹药不足 / 未到射速间隔而被拒绝）。
    // 这里刻意不据此做任何补偿：拒绝就是拒绝（客户端也已自行做过"能否开火"的本地判断用于表现），
    // 服务器只管裁决，不负责纠正客户端表现 —— 表现与判定的最终一致性由复制状态（弹药/换弹）收敛。
    const bool bFired = WeaponComponent->TryFire(AimLocation, AimRotation);

    UE_LOG(LogZombieKing, Verbose, TEXT("[远程攻击技能] %s 开火请求：%s"),
        *AvatarActor->GetName(), bFired ? TEXT("已开火") : TEXT("被拒绝（换弹中/弹药不足/未到射速间隔）"));

    // 单发结算完成即结束技能（连续射击由输入层按射速节奏反复请求激活，见 Lua）
    EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}
