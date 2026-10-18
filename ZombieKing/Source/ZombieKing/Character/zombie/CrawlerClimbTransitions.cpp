#include "CrawlerClimbTransitions.h"
#include "CrawlerClimbMode.h"
#include "MoveLibrary/MovementUtils.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"

#if ENABLE_DRAW_DEBUG
#include "DrawDebugHelpers.h"
#endif

FTransitionEvalResult UCrawlerAttachTransition::Evaluate_Implementation(const FSimulationTickParams& Params) const
{
    // -------------------------------------------------------------------------
    // 1. 模式门控：仅从"地面行走 / 空中下落"切入爬墙。
    //    爬墙中（Climbing）不重复评估；飞行/游泳等模式不属于多足生化角色的能力范围。
    // -------------------------------------------------------------------------
    const FName CurrentMode = Params.StartState.SyncState.MovementMode;
    if (CurrentMode != DefaultModeNames::Walking && CurrentMode != DefaultModeNames::Falling)
    {
        return FTransitionEvalResult::NoTransition;
    }

    USceneComponent* UpdatedComponent = Params.MovingComps.UpdatedComponent.Get();
    if (!UpdatedComponent)
    {
        return FTransitionEvalResult::NoTransition;
    }

    UWorld* World = UpdatedComponent->GetWorld();
    if (!World)
    {
        return FTransitionEvalResult::NoTransition;
    }

    // -------------------------------------------------------------------------
    // 2. 视线方向：本地玩家取自输入指令的相机朝向（随预测复制，各端一致）；
    //    AI 控制器没有相机概念，回退为角色当前前向（生化 AI 同样支持自动吸附）。
    // -------------------------------------------------------------------------
    const FCharacterDefaultInputs* CharacterInputs = Params.StartState.InputCmd.InputCollection.FindDataByType<FCharacterDefaultInputs>();

    FVector ViewDirection = UpdatedComponent->GetForwardVector();
    if (CharacterInputs && !CharacterInputs->ControlRotation.IsNearlyZero())
    {
        ViewDirection = CharacterInputs->ControlRotation.Vector().GetSafeNormal();
    }

    // -------------------------------------------------------------------------
    // 3. 从角色中心沿视线方向射线检测（"角色靠近可攀爬墙面时进行射线检测"）
    // -------------------------------------------------------------------------
    const FVector TraceStart = UpdatedComponent->GetComponentLocation();
    const FVector TraceEnd = TraceStart + ViewDirection * AttachMaxDistance;

    FCollisionQueryParams QueryParams;
    FCollisionResponseParams ResponseParams;
    UMovementUtils::InitCollisionParams(Params.MovingComps.UpdatedPrimitive.Get(), QueryParams, ResponseParams);
    QueryParams.AddIgnoredActor(UpdatedComponent->GetOwner());

    FHitResult Hit;
    // 使用 Pawn 对象通道（与 Mover 地面检测一致：静态墙体默认阻挡 Pawn）
    const bool bHit = World->LineTraceSingleByChannel(Hit, TraceStart, TraceEnd, ECC_Pawn, QueryParams, ResponseParams);

#if ENABLE_DRAW_DEBUG
    if (bDrawDebugAttachTrace)
    {
        DrawDebugLine(World, TraceStart, TraceEnd, bHit ? FColor::Yellow : FColor::Silver, false, -1.0f, 0, 1.5f);
    }
#endif

    if (!bHit)
    {
        return FTransitionEvalResult::NoTransition;
    }

    // 4. 合理距离：命中距离不得超过配置上限（射线长度已限制，这里再校验一次命中距离字段，
    //    兼容未来替换为球体扫描等多形态检测的情况）
    if (Hit.Distance > AttachMaxDistance)
    {
        return FTransitionEvalResult::NoTransition;
    }

    // -------------------------------------------------------------------------
    // 5. 可攀爬坡面校验：仅"接近垂直的墙面"（默认 >= 70°），天花板/缓坡/地面不可吸附
    // -------------------------------------------------------------------------
    const FVector WallNormal = Hit.ImpactNormal;
    const float VerticalDot = FMath::Clamp(FMath::Abs(WallNormal.Z), 0.0f, 1.0f);
    const float SlopeAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(VerticalDot));
    if (SlopeAngleDegrees < MinClimbableSlopeAngle)
    {
        return FTransitionEvalResult::NoTransition;
    }

    // -------------------------------------------------------------------------
    // 6. 摄像机面向墙面校验：dot(视线, -墙面法线) >= MinFacingWallDot。
    //    默认阈值 0.5（cos60°）等价于"视线与墙面表面的夹角 >= 30°"——
    //    擦着墙面跑过（视线几乎平行于墙面）不会触发误吸附，必须真正"看着墙"才吸附。
    // -------------------------------------------------------------------------
    const float FacingDot = FVector::DotProduct(ViewDirection, -WallNormal);
    if (FacingDot < MinFacingWallDot)
    {
        return FTransitionEvalResult::NoTransition;
    }

#if ENABLE_DRAW_DEBUG
    if (bDrawDebugAttachTrace)
    {
        DrawDebugPoint(World, Hit.ImpactPoint, 14.0f, FColor::Orange, false, -1.0f, 0);
        DrawDebugDirectionalArrow(World, Hit.ImpactPoint, Hit.ImpactPoint + WallNormal * 60.0f, 12.0f, FColor::Cyan, false, -1.0f, 0, 2.0f);
    }
#endif

    // -------------------------------------------------------------------------
    // 7. 全部条件通过：请求切入爬墙模式。
    //    Mover 状态机将在本帧随模拟原子地完成模式切换（预测安全）。
    // -------------------------------------------------------------------------
    return FTransitionEvalResult(CrawlerModeNames::Climbing);
}
