#include "GameMeleeAttackAbility.h"
#include "Engine/World.h"              // GetWorld()->SweepMultiByChannel
#include "CollisionQueryParams.h"      // FCollisionQueryParams / SCENE_QUERY_STAT
#include "CollisionShape.h"            // FCollisionShape::MakeSphere
#include "Engine/EngineTypes.h"        // FCollisionShape / ECC_Pawn
#include "DrawDebugHelpers.h"          // DrawDebugSphere / DrawDebugLine（仅开发期）
#include "ZombieKing.h"                // LogZombieKing（异常告警）

UGameMeleeAttackAbility::UGameMeleeAttackAbility()
{
    // 近战为"一次结算型"技能，无跨帧状态，沿用基类的 ServerOnly 策略即可（此处仅显式说明意图）：
    // 判定与伤害全部在服务器完成，客户端只做表现（由角色 Lua 在输入回调中播放蒙太奇）。
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
}

void UGameMeleeAttackAbility::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
                                              const FGameplayAbilityActorInfo* ActorInfo,
                                              const FGameplayAbilityActivationInfo ActivationInfo,
                                              const FGameplayEventData* TriggerEventData)
{
    // 双保险：即便技能被误配为可在客户端执行，也不允许客户端裁定伤害
    if (!K2_HasAuthority())
    {
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/true);
        return;
    }

    // 提交技能：应用其消耗与冷却（若技能蓝图未配置消耗/冷却，则本调用为空操作并返回 true）
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
    {
        // 提交失败 = 冷却未好 / 消耗不足：正常结束，不算取消
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
        return;
    }

    AActor* AvatarActor = GetAvatarActorFromActorInfo();
    UWorld* World = GetWorld();
    if (!AvatarActor || !World)
    {
        // 缺少 Avatar（例如技能在无 Pawn 状态被激活）：直接结束，避免空指针
        EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
        return;
    }

    // -------------------------------------------------------------------------
    // 1) 计算扫掠区间：从"瞄准起点"沿准星方向向前延伸 MeleeRange
    //    起点取相机/眼睛位置，保证"看到的准星方向"与"爪击方向"完全一致。
    // -------------------------------------------------------------------------
    FVector AimLocation;
    FRotator AimRotation;
    GetAttackAimViewPoint(AimLocation, AimRotation);

    const FVector SweepStart = AimLocation;
    const FVector SweepEnd = SweepStart + AimRotation.Vector() * MeleeRange;

    // -------------------------------------------------------------------------
    // 2) 球体扫掠：用半径把判定从"一条线"扩成"一个体积"，匹配爪击的覆盖范围
    //    参数：忽略自身（避免自伤/自阻挡）；bFindInitialOverlaps 默认开启，
    //          因此"贴身零距离"的敌人（球体起点即与目标胶囊重叠）也能被扫到。
    // -------------------------------------------------------------------------
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GameMeleeAttack), /*bTraceComplex=*/false, AvatarActor);

    TArray<FHitResult> HitResults;
    World->SweepMultiByChannel(
        HitResults, SweepStart, SweepEnd, FQuat::Identity,
        TraceChannel, FCollisionShape::MakeSphere(MeleeRadius), QueryParams);

    // -------------------------------------------------------------------------
    // 3) 逐个结算：同一次扫掠可能对同一目标产生多个命中（多段），用集合去重保证"一次攻击一次伤害"
    // -------------------------------------------------------------------------
    TSet<AActor*> DamagedActors;
    int32 DamagedCount = 0;

    for (const FHitResult& Hit : HitResults)
    {
        AActor* HitActor = Hit.GetActor();
        if (!HitActor || DamagedActors.Contains(HitActor))
        {
            continue;
        }

        // 阵营/合法性过滤在基类 ApplyDamageToTarget 内统一完成（此处先记录去重，避免重复校验开销）
        DamagedActors.Add(HitActor);

        // 近战伤害置 bMeleeDamage=true：受击方据此缓存"最近一次受击是否近战"，
        // Lua 死亡规则实现 CF 式"刀杀禁令"（被刀杀的生化幽灵本回合禁复活进观战）
        if (ApplyDamageToTarget(HitActor, MeleeDamage, DamageEffectClass, &Hit, /*bMeleeDamage=*/true))
        {
            ++DamagedCount;
        }
    }

#if ENABLE_DRAW_DEBUG
    if (bDrawDebug)
    {
        // 开发期可视化：把扫掠轨迹与实际命中点画出来，便于调范围/半径
        DrawDebugLine(World, SweepStart, SweepEnd, FColor::Yellow, false, 1.0f, 0, 1.5f);
        DrawDebugSphere(World, SweepEnd, MeleeRadius, 12, FColor::Orange, false, 1.0f);
        for (const FHitResult& Hit : HitResults)
        {
            DrawDebugPoint(World, Hit.ImpactPoint, 8.0f, FColor::Red, false, 1.0f);
        }
    }
#endif

    UE_LOG(LogZombieKing, Verbose, TEXT("[%s] 近战爪击结算：命中 %d 个敌对目标（扫掠半径 %.1f，距离 %.1f）"),
        *GetName(), DamagedCount, MeleeRadius, MeleeRange);

    // -------------------------------------------------------------------------
    // 4) 结束技能：单次结算型技能无需等待，立即结束并复制结束状态
    //    说明：各端的表现（挥击蒙太奇、受击火花）分别由攻击方/受击方的 Lua 自行处理。
    // -------------------------------------------------------------------------
    EndAbility(Handle, ActorInfo, ActivationInfo, /*bReplicateEndAbility=*/true, /*bWasCancelled=*/false);
}
