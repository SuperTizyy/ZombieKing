#include "CrawlerClimbMode.h"
#include "MoverComponent.h"
#include "MoveLibrary/MovementUtils.h"
#include "MoveLibrary/MoverBlackboard.h"
#include "NativeGameplayTags.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"

#if ENABLE_DRAW_DEBUG
#include "DrawDebugHelpers.h"
#endif

// 原生声明运动模式标签（无需 ini 注册，CDO 构造期即可安全使用）
UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_Movement_Mode_Climbing, "Movement.Mode.Climbing");

namespace CrawlerClimbBlackboard
{
    /** 黑板键：最近一次爬墙表面命中结果（供调试/HUD/Lua 查询，不参与权威逻辑） */
    const FName ClimbSurfaceHit = TEXT("CrawlerClimbSurfaceHit");
}

UCrawlerClimbMode::UCrawlerClimbMode()
{
    // 默认运动模式标签：供游戏逻辑（如动画状态机/技能判定）过滤"当前是否在爬墙"
    GameplayTags.AddTag(TAG_Movement_Mode_Climbing);
}

bool UCrawlerClimbMode::IsClimbableSurface(const FVector& SurfaceNormal) const
{
    // 坡面角度定义：法线与竖直方向的夹角（0° = 水平地面/天花板，90° = 垂直墙面）。
    // 取绝对值使"正反两面"对称处理；MinClimbableSlopeAngle（默认 70°）以上才可攀爬，
    // 以此落实"仅接近垂直的墙面可爬、天花板与大坡地面不可爬"的玩法设计。
    const float VerticalDot = FMath::Clamp(FMath::Abs(SurfaceNormal.Z), 0.0f, 1.0f);
    const float SlopeAngleDegrees = FMath::RadiansToDegrees(FMath::Acos(VerticalDot));
    return SlopeAngleDegrees >= MinClimbableSlopeAngle;
}

bool UCrawlerClimbMode::DetectSurfaceNormal(const FMovingComponentSet& MovingComps,
                                            FVector& OutSurfaceNormal,
                                            FVector& OutSurfaceHitPoint,
                                            float& OutHitDistance,
                                            FHitResult& OutHitResult) const
{
    USceneComponent* UpdatedComponent = MovingComps.UpdatedComponent.Get();
    if (!UpdatedComponent)
    {
        return false;
    }

    UWorld* World = UpdatedComponent->GetWorld();
    if (!World)
    {
        return false;
    }

    // 沿角色腹部方向（局部 -Up）探测：爬墙姿态下角色 Up = 墙面法线，因此 -Up 即指向墙面。
    const FVector DownDirection = -UpdatedComponent->GetUpVector();
    const FVector Origin = UpdatedComponent->GetComponentLocation();
    const FVector Fwd = UpdatedComponent->GetForwardVector();
    const FVector Right = UpdatedComponent->GetRightVector();

    // 多发线采样：中心 + 前/后/左/右偏移点。
    // 目的：跨越凸边（如窗台/门框）时中心射线可能落空，但偏移射线仍能锁定墙面，避免误脱离。
    TArray<FVector, TInlineAllocator<5>> TraceOrigins;
    TraceOrigins.Add(Origin);
    TraceOrigins.Add(Origin + Fwd * ProbeOffset);
    TraceOrigins.Add(Origin - Fwd * ProbeOffset);
    TraceOrigins.Add(Origin + Right * ProbeOffset);
    TraceOrigins.Add(Origin - Right * ProbeOffset);

    // 复用 Mover 的碰撞参数初始化（继承被移动组件的碰撞响应，忽略自身）
    FCollisionQueryParams QueryParams;
    FCollisionResponseParams ResponseParams;
    UMovementUtils::InitCollisionParams(MovingComps.UpdatedPrimitive.Get(), QueryParams, ResponseParams);
    QueryParams.AddIgnoredActor(UpdatedComponent->GetOwner());

    bool bFoundAnyValid = false;
    float BestDistance = TNumericLimits<float>::Max();
    FHitResult BestHit;

    for (const FVector& Start : TraceOrigins)
    {
        const FVector End = Start + DownDirection * SurfaceTraceDistance;

        FHitResult Hit;
        // 使用 Pawn 对象通道做线检测（与 Mover 地面检测一致：Static/WorldStatic 默认阻挡 Pawn）
        const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, ECC_Pawn, QueryParams, ResponseParams);

#if ENABLE_DRAW_DEBUG
        if (bDrawDebugClimbTrace)
        {
            DrawDebugLine(World, Start, End, bHit ? FColor::Green : FColor::Red, false, -1.0f, 0, 1.5f);
        }
#endif

        if (!bHit)
        {
            continue;
        }

        // 仅接受"近似垂直"的表面：地面/天花板/缓坡一律视为不可攀爬
        if (!IsClimbableSurface(Hit.ImpactNormal))
        {
            continue;
        }

        // 在全部合格命中中取最近者（最贴近当前贴附面的那一面墙）
        if (Hit.Distance < BestDistance)
        {
            BestDistance = Hit.Distance;
            BestHit = Hit;
            bFoundAnyValid = true;
        }
    }

    if (!bFoundAnyValid)
    {
        return false;
    }

    OutSurfaceNormal = BestHit.ImpactNormal;
    OutSurfaceHitPoint = BestHit.ImpactPoint;
    OutHitDistance = BestDistance;
    OutHitResult = BestHit;

#if ENABLE_DRAW_DEBUG
    if (bDrawDebugClimbTrace)
    {
        DrawDebugDirectionalArrow(World, OutSurfaceHitPoint, OutSurfaceHitPoint + OutSurfaceNormal * 60.0f, 12.0f, FColor::Cyan, false, -1.0f, 0, 2.0f);
    }
#endif

    return true;
}

// =============================================================================
//  GenerateMove：把输入指令换算为世界空间期望速度
// =============================================================================

void UCrawlerClimbMode::GenerateMove_Implementation(const FMoverTickStartData& StartState, const FMoverTimeStep& TimeStep, FProposedMove& OutProposedMove) const
{
    const FCharacterDefaultInputs* CharacterInputs = StartState.InputCmd.InputCollection.FindDataByType<FCharacterDefaultInputs>();

    FVector DesiredVelocity = FVector::ZeroVector;
    if (CharacterInputs)
    {
        // 基类 OnProduceInput 已按相机朝向把输入换算为世界空间方向（DirectionalIntent 模长 0..1）。
        // 这里只做"意图 → 速度"的换算，切平面投影统一放在 SimulationTick（需要墙面法线）。
        const FVector& MoveInput = CharacterInputs->GetMoveInput();
        if (CharacterInputs->GetMoveInputType() == EMoveInputType::Velocity)
        {
            // 绝对速度输入：只做上限截断
            DesiredVelocity = MoveInput.GetClampedToMaxSize(MaxClimbSpeed);
        }
        else
        {
            // 方向意图输入：模长代表期望速度比例
            DesiredVelocity = MoveInput.GetClampedToMaxSize(1.0f) * MaxClimbSpeed;
        }
    }

    OutProposedMove.LinearVelocity = DesiredVelocity;
    OutProposedMove.DirectionIntent = DesiredVelocity.GetSafeNormal();
    OutProposedMove.bHasDirIntent = !DesiredVelocity.IsNearlyZero();
}

// =============================================================================
//  SimulationTick：表面探测 → 速度解算 → 姿态对齐 → 安全位移 → 提交状态
// =============================================================================

void UCrawlerClimbMode::SimulationTick_Implementation(const FSimulationTickParams& Params, FMoverTickEndData& OutputState)
{
    UMoverComponent* MoverComp = GetMoverComponent();
    USceneComponent* UpdatedComponent = Params.MovingComps.UpdatedComponent.Get();
    if (!MoverComp || !UpdatedComponent)
    {
        return;
    }

    // 起始与输出同步状态（Mover 标准数据块：位置/朝向/速度）
    const FMoverDefaultSyncState* StartingSyncState = Params.StartState.SyncState.SyncStateCollection.FindDataByType<FMoverDefaultSyncState>();
    check(StartingSyncState);
    FMoverDefaultSyncState& OutputSyncState = OutputState.SyncState.SyncStateCollection.FindOrAddMutableDataByType<FMoverDefaultSyncState>();

    const float DeltaSeconds = Params.TimeStep.StepMs * 0.001f;
    const FCharacterDefaultInputs* CharacterInputs = Params.StartState.InputCmd.InputCollection.FindDataByType<FCharacterDefaultInputs>();
    const FVector PriorVelocity = StartingSyncState->GetVelocity_WorldSpace();
    const FVector GravityAccel = MoverComp->GetGravityAcceleration();

    FMovementRecord MoveRecord;
    MoveRecord.SetDeltaSeconds(DeltaSeconds);

    // -------------------------------------------------------------------------
    // 1. 表面探测（墙面法线/命中结果同时缓存进黑板，供调试面板与 Lua 查询）
    // -------------------------------------------------------------------------
    FVector SurfaceNormal = FVector::UpVector;
    FVector SurfaceHitPoint = FVector::ZeroVector;
    float HitDistance = 0.0f;
    FHitResult SurfaceHit;
    const bool bOnSurface = DetectSurfaceNormal(Params.MovingComps, SurfaceNormal, SurfaceHitPoint, HitDistance, SurfaceHit);

    UMoverBlackboard* SimBlackboard = MoverComp->GetSimBlackboard_Mutable();
    if (SimBlackboard)
    {
        if (bOnSurface)
        {
            SimBlackboard->Set(CrawlerClimbBlackboard::ClimbSurfaceHit, SurfaceHit);
        }
        else
        {
            SimBlackboard->Invalidate(CrawlerClimbBlackboard::ClimbSurfaceHit);
        }
    }

    const bool bJumpJustPressed = CharacterInputs && CharacterInputs->bIsJumpJustPressed;

    // -------------------------------------------------------------------------
    // 2a. 蹬墙弹射：按下跳跃 → 沿墙面法线（含可配向上分量）跃出，切回 Falling
    //     速度直接写入本帧同步状态，下一帧起由 Falling 模式接管（动量完整保留）。
    // -------------------------------------------------------------------------
    if (bJumpJustPressed)
    {
        // 若恰在边缘瞬时丢面，回退用角色当前 Up 作为弹射方向，避免零向量
        const FVector LaunchNormal = bOnSurface ? SurfaceNormal : UpdatedComponent->GetUpVector();
        const FVector LaunchVelocity = LaunchNormal * JumpOffSpeed + FVector::UpVector * JumpOffUpSpeed;

        OutputSyncState = *StartingSyncState;
        OutputSyncState.SetTransforms_WorldSpace(
            UpdatedComponent->GetComponentLocation(),
            UpdatedComponent->GetComponentRotation(),
            LaunchVelocity,
            FVector::ZeroVector,
            nullptr,
            NAME_None);
        UpdatedComponent->ComponentVelocity = LaunchVelocity;

        // 切换回下落模式：引擎标准做法是在 MovementEndState 中声明下一模式，
        // 本帧模拟结束后由状态机原子切换（比 QueueNextMode 的"下一帧排队"更及时，且随预测/回滚一致）
        OutputState.MovementEndState.NextModeName = DefaultModeNames::Falling;
        return;
    }

    // -------------------------------------------------------------------------
    // 2b. 丢失表面：保留当前动量直坠，切回 Falling（引擎在落地后自动恢复 Walking）
    // -------------------------------------------------------------------------
    if (!bOnSurface)
    {
        const FVector FallVelocity = PriorVelocity + GravityAccel * DeltaSeconds;
        const FQuat CurrentRotation = UpdatedComponent->GetComponentQuat();
        const FVector FallDelta = FallVelocity * DeltaSeconds;

        FHitResult Hit(1.0f);
        UMovementUtils::TrySafeMoveUpdatedComponent(Params.MovingComps, FallDelta, CurrentRotation, true, Hit, ETeleportType::None, MoveRecord);

        const FVector FinalVelocity = MoveRecord.GetRelevantVelocity();
        OutputSyncState = *StartingSyncState;
        OutputSyncState.SetTransforms_WorldSpace(
            UpdatedComponent->GetComponentLocation(),
            UpdatedComponent->GetComponentRotation(),
            FinalVelocity,
            FVector::ZeroVector,
            nullptr,
            NAME_None);
        UpdatedComponent->ComponentVelocity = FinalVelocity;

        // 失去表面：本帧动量已写入同步状态，声明切回下落模式（引擎标准切换方式）
        OutputState.MovementEndState.NextModeName = DefaultModeNames::Falling;
        return;
    }

    // -------------------------------------------------------------------------
    // 3. 贴墙爬行：速度解算
    //    - 输入速度投影到墙面切线平面（不会往墙体内部挤压）
    //    - 重力切向分量 × (1 - GripStrength) 作为可配滑落速度
    //    - 沿法线反方向的吸附推力，保证凸边/台阶过渡时不脱墙
    // -------------------------------------------------------------------------
    const FVector DesiredVelocity = Params.ProposedMove.LinearVelocity;
    const FVector TangentVelocity = FVector::VectorPlaneProject(DesiredVelocity, SurfaceNormal);

    const FVector GravityTangential = FVector::VectorPlaneProject(GravityAccel, SurfaceNormal);
    const FVector SlideVelocity = GravityTangential * (1.0f - FMath::Clamp(GripStrength, 0.0f, 1.0f));

    const FVector AdhesionVelocity = -SurfaceNormal * SurfaceAdhesionStrength;

    const FVector FinalVelocity = TangentVelocity + SlideVelocity + AdhesionVelocity;

    // -------------------------------------------------------------------------
    // 4. 姿态对齐：角色 Up 对齐墙面法线（腹部贴墙），前向取"相机前向在墙面上的投影"
    // -------------------------------------------------------------------------
    FRotator CurrentOrientation = StartingSyncState->GetOrientation_WorldSpace();

    FVector DesiredFacing = CurrentOrientation.Vector();
    if (CharacterInputs && !CharacterInputs->ControlRotation.IsNearlyZero())
    {
        // 相机朝向来自输入指令（随预测复制），保证各端计算一致
        DesiredFacing = CharacterInputs->ControlRotation.Vector();
    }

    FVector TangentFacing = FVector::VectorPlaneProject(DesiredFacing, SurfaceNormal).GetSafeNormal();
    if (TangentFacing.IsNearlyZero())
    {
        // 相机正中直墙面导致投影退化时，回退为当前朝向的投影
        TangentFacing = FVector::VectorPlaneProject(CurrentOrientation.Vector(), SurfaceNormal).GetSafeNormal();
    }
    if (TangentFacing.IsNearlyZero())
    {
        // 极端兜底：使用墙面上的任意稳定切线（利用世界 Up 构造正交基）
        TangentFacing = FVector::CrossProduct(SurfaceNormal, FVector::UpVector).GetSafeNormal();
        if (TangentFacing.IsNearlyZero())
        {
            TangentFacing = FVector::CrossProduct(SurfaceNormal, FVector::ForwardVector).GetSafeNormal();
        }
    }

    const FQuat TargetRotation = FRotationMatrix::MakeFromZX(SurfaceNormal, TangentFacing).ToQuat();

    // -------------------------------------------------------------------------
    // 5. 位移执行：安全移动 + 阻挡时沿表面滑动（Mover 标准工具链，自动记录 FMovementRecord）
    // -------------------------------------------------------------------------
    const FVector MoveDelta = FinalVelocity * DeltaSeconds;
    FHitResult Hit(1.0f);
    UMovementUtils::TrySafeMoveUpdatedComponent(Params.MovingComps, MoveDelta, TargetRotation, true, Hit, ETeleportType::None, MoveRecord);

    if (Hit.IsValidBlockingHit())
    {
        // 通知 Mover 管线发生碰撞（音效/特效/受击检查等可订阅该事件）
        FMoverOnImpactParams ImpactParams(CrawlerModeNames::Climbing, Hit, MoveDelta);
        MoverComp->HandleImpact(ImpactParams);

        // 剩余距离沿墙面滑动（门窗框/拐角处自动扫掠绕行）
        UMovementUtils::TryMoveToSlideAlongSurface(Params.MovingComps, MoveDelta, 1.0f - Hit.Time, TargetRotation, Hit.Normal, Hit, true, MoveRecord);
    }

    // -------------------------------------------------------------------------
    // 6. 提交同步状态：位置/朝向/速度写回 Mover（负责复制、插值与回滚校验）
    // -------------------------------------------------------------------------
    const FVector FinalLocation = UpdatedComponent->GetComponentLocation();
    const FVector RecordVelocity = MoveRecord.GetRelevantVelocity();

    OutputSyncState.SetTransforms_WorldSpace(
        FinalLocation,
        UpdatedComponent->GetComponentRotation(),
        RecordVelocity,
        FVector::ZeroVector,
        nullptr,
        NAME_None);

    // 物理体速度同步（供动画蓝图/根骨骼运动参考）
    UpdatedComponent->ComponentVelocity = RecordVelocity;
}
