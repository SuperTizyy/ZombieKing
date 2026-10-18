#include "GameAnimInstanceBase.h"
#include "GameCharacterBase.h"
#include "DefaultMovementSet/CharacterMoverComponent.h"
#include "DefaultMovementSet/Settings/CommonLegacyMovementSettings.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/BlendSpace.h"

UGameAnimInstanceBase::UGameAnimInstanceBase()
{
    // 默认绑定通用动画脚本；专属动画实例子类（如 UZombieScorpionAnimInstance）或具体动画蓝图可换绑脚本
    LuaScriptPath = TEXT("Lua.Core.Animation.GameAnimInstanceBase");
}

FString UGameAnimInstanceBase::GetModuleName_Implementation() const
{
    // 与 AGameCharacterBase 相同：优先使用配置里的脚本模块路径，实现"动画逻辑热更新"
    return LuaScriptPath;
}

// =============================================================================
//  初始化
// =============================================================================

void UGameAnimInstanceBase::NativeInitializeAnimation()
{
    Super::NativeInitializeAnimation();

    // 1. 缓存宿主角色与 Mover 组件（本类只服务于基于 AGameCharacterBase 的 Mover 角色）
    OwningCharacter = Cast<AGameCharacterBase>(TryGetPawnOwner());
    if (!OwningCharacter)
    {
        UE_LOG(LogTemp, Error, TEXT("[%s] 动画实例宿主不是 AGameCharacterBase：请确认该动画蓝图仅用于本项目的生化角色网格"), *GetName());
        return;
    }

    MoverComponent = OwningCharacter->MoverComponent;
    if (!MoverComponent)
    {
        UE_LOG(LogTemp, Error, TEXT("[%s] 宿主角色 %s 未挂载 Mover 组件，动画数据桥不可用"), *GetName(), *OwningCharacter->GetName());
        return;
    }

    // 2. 读取 Mover 共享设置（每角色不同的 MaxSpeed，供速度归一化 / SpeedRatio 使用）
    //    数据来源：MoverComponent 的 SharedSettings → UCommonLegacyMovementSettings
    MovementSettings = MoverComponent->FindSharedSettings_Mutable<UCommonLegacyMovementSettings>();
    if (MovementSettings)
    {
        MaxSpeed = MovementSettings->MaxSpeed;
    }
    else
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未找到 CommonLegacyMovementSettings：MaxSpeed 保持 0，速度归一化（SpeedRatio）将不可用"), *GetName());
    }

    // 3. 动画槽位自诊断（延续项目"资产未配置显式告警"规范）：
    //    槽位为空不崩溃，但对应状态会播放绑定姿态（T-Pose），必须第一时间暴露出来
    if (!Anim_Idle)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未配置 Anim_Idle：Idle 状态将播放绑定姿态（请在动画蓝图 Class Defaults → Anim|Slots 中赋值）"), *GetName());
    }
    if (!LocomotionBlendSpace)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未配置 LocomotionBlendSpace：地面移动状态将播放绑定姿态（请在动画蓝图 Class Defaults → Anim|Slots 中赋值）"), *GetName());
    }
    if (!Anim_AirLoop)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未配置 Anim_AirLoop：腾空状态将播放绑定姿态（请在动画蓝图 Class Defaults → Anim|Slots 中赋值）"), *GetName());
    }

    // 4. 模式名基线：避免首帧把"初始模式"误报成一次模式切换事件
    LastMovementModeName = MoverComponent->GetMovementModeName();

    // 5. 派发 Lua 初始化事件（热更新调参入口：Lua 在此覆盖阈值/参考速度等，
    //    必须放在 C++ 默认值就绪之后、正式播放之前）
    OnAnimInstanceInitialized();
}

// =============================================================================
//  每帧数据刷新（游戏线程）
// =============================================================================

void UGameAnimInstanceBase::NativeUpdateAnimation(float DeltaSeconds)
{
    Super::NativeUpdateAnimation(DeltaSeconds);

    // 纯数据刷新：NativeUpdateAnimation 保证在游戏线程执行，动画图工作线程只读取
    // 属性快照；此处严禁调用 Lua（性能红线），Lua 事件仅在初始化与模式变化边沿派发
    RefreshMovementState(DeltaSeconds);
}

void UGameAnimInstanceBase::RefreshMovementState(float /*DeltaSeconds*/)
{
    if (!MoverComponent || !OwningCharacter)
    {
        return;
    }

    // 1. 速度：Mover 同步状态为唯一权威来源（随预测/回滚一致复制，
    //    禁止改读 Actor 物理速度/上一帧位移差——那些在重模拟下会产生动画抖动）
    WorldVelocity = MoverComponent->GetVelocity();

    // 2. 转换到机身坐标系：地面时 ≈ 世界平面速度；贴墙爬行时 = 沿墙面平面速度
    //    （爬墙模式已把机身 Up 对齐墙面法线，机身平面即爬行平面，多足/双足统一语义）
    const FVector LocalVelocity = OwningCharacter->GetActorRotation().UnrotateVector(WorldVelocity);
    VelocityZ = LocalVelocity.Z;

    const FVector2D LocalPlanarVelocity(LocalVelocity.X, LocalVelocity.Y);
    Speed = LocalPlanarVelocity.Size();
    VelocityLocalDir = Speed > UE_KINDA_SMALL_NUMBER ? LocalPlanarVelocity / Speed : FVector2D::ZeroVector;

    // 3. 移动判定与归一化（阈值/参考速度均可在 Lua 热更调整）
    bIsMoving = Speed > MovingSpeedThreshold;
    SpeedRatio = MaxSpeed > UE_KINDA_SMALL_NUMBER ? FMath::Clamp(Speed / MaxSpeed, 0.0f, 1.0f) : 0.0f;

    // 4. 运动状态（全部取自 Mover 组件，双足/多足/爬墙统一语义）
    bIsOnGround = MoverComponent->IsOnGround();
    bIsAirborne = MoverComponent->IsAirborne();
    bIsFalling = MoverComponent->IsFalling();
    bIsFlying = MoverComponent->IsFlying();

    // 5. 运动模式名 + 模式变化边沿事件（仅模式切换时派发 Lua 表现事件，非每帧）
    MovementModeName = MoverComponent->GetMovementModeName();
    if (MovementModeName != LastMovementModeName)
    {
        const FName PreviousModeName = LastMovementModeName;
        LastMovementModeName = MovementModeName;
        OnAnimMovementModeChanged(PreviousModeName, MovementModeName);
    }
}
