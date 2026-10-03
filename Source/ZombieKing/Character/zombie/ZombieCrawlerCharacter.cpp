#include "ZombieCrawlerCharacter.h"
#include "CrawlerMoverComponent.h"
#include "CrawlerClimbMode.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Components/CapsuleComponent.h"

AZombieCrawlerCharacter::AZombieCrawlerCharacter(const FObjectInitializer& ObjectInitializer)
    // 关键：把基类挂的通用 MoverComponent 替换为多足专属组件。
    // 子类组件在构造阶段完成"爬墙模式 + 全局吸附过渡"的组装（官方推荐的模式注册方式），
    // 组件名必须与基类 CreateDefaultSubobject 使用的名字（"MoverComponent"）一致。
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UCrawlerMoverComponent>(TEXT("MoverComponent")))
{
    // 1. 指定绑定的 UnLua 脚本路径（相对 Content/Script，文件位于 Lua/Core/Character/zombie/）
    LuaScriptPath = TEXT("Lua.Core.Character.zombie.ZombieCrawlerCharacter");

    // 2. 调整原生胶囊体为"扁平/矮粗"形态，适配多足蜘蛛/异形底盘
    if (UCapsuleComponent* Capsule = Cast<UCapsuleComponent>(GetRootComponent()))
    {
        Capsule->SetCapsuleHalfHeight(45.0f); // 降低高度
        Capsule->SetCapsuleRadius(60.0f);     // 增大半径，包裹多足腹部
    }

    // 3. 配置真第一人称相机头部挂点：多足生物眼睛处于头胸部前侧
    HeadBoneName = FName("Spider_Head");

    // 4. 自由视角：Mover 由同步状态驱动角色姿态（爬墙姿态 = Up 对齐墙面法线），
    //    禁止 Controller 旋转直接改写 Actor 朝向，避免与运动模式姿态解算打架
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;
}

void AZombieCrawlerCharacter::BeginPlay()
{
    Super::BeginPlay();

    // 监听运动模式变化：向 Lua 广播"进出爬墙"表现事件。
    // 注意：本角色在上层只需要这一层薄转发——爬墙模式与吸附过渡均已由
    // UCrawlerMoverComponent 在组件构造期注册完毕，无需在此做任何运动模式注册。
    if (MoverComponent)
    {
        MoverComponent->OnMovementModeChanged.AddDynamic(this, &AZombieCrawlerCharacter::HandleMovementModeChanged);
    }
}

bool AZombieCrawlerCharacter::IsClimbing() const
{
    return MoverComponent && MoverComponent->GetMovementModeName() == CrawlerModeNames::Climbing;
}

void AZombieCrawlerCharacter::OnProduceInput(float DeltaMs, FMoverInputCmdContext& OutInputCmd)
{
    // 先走基类的通用输入填充（相机朝向 / 世界空间移动意图 / 跳跃标志）
    Super::OnProduceInput(DeltaMs, OutInputCmd);

    // 多足角色地面朝向跟随相机：让地面行走时始终面朝视线方向，W 始终朝准星方向推进。
    // （爬墙姿态由 UCrawlerClimbMode 在模拟内单独解算：Up = 墙面法线、前向 = 相机前向的面投影）
    if (GetController())
    {
        if (FCharacterDefaultInputs* Inputs = OutInputCmd.InputCollection.FindMutableDataByType<FCharacterDefaultInputs>())
        {
            Inputs->OrientationIntent = Inputs->ControlRotation.Vector().GetSafeNormal();
        }
    }
}

void AZombieCrawlerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    // 激活 Enhanced Input 映射上下文
    if (APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            if (CrawlerMappingContext)
            {
                Subsystem->AddMappingContext(CrawlerMappingContext, 0);
            }
        }
    }

    // 绑定基础输入动作
    UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!EnhancedInputComponent)
    {
        return;
    }

    if (ActionMove)
    {
        EnhancedInputComponent->BindAction(ActionMove, ETriggerEvent::Triggered, this, &AZombieCrawlerCharacter::Input_Move);
    }
    if (ActionLook)
    {
        EnhancedInputComponent->BindAction(ActionLook, ETriggerEvent::Triggered, this, &AZombieCrawlerCharacter::Input_Look);
    }
    if (ActionAttack)
    {
        EnhancedInputComponent->BindAction(ActionAttack, ETriggerEvent::Started, this, &AZombieCrawlerCharacter::Input_Attack);
    }
    if (ActionJump)
    {
        EnhancedInputComponent->BindAction(ActionJump, ETriggerEvent::Started, this, &AZombieCrawlerCharacter::Input_Jump);
        EnhancedInputComponent->BindAction(ActionJump, ETriggerEvent::Completed, this, &AZombieCrawlerCharacter::Input_JumpReleased);
    }
}

void AZombieCrawlerCharacter::Input_Move(const FInputActionValue& Value)
{
    // 增强输入 IA 轴约定（与引擎模板一致）：X = 左/右，Y = 前/后
    const FVector2D MovementVector = Value.Get<FVector2D>();

    // 缓存为"本地意图"（X = 前后, Y = 左右，取值 -1..1）。
    // 世界空间换算（按相机朝向）统一在基类 OnProduceInput 中完成，
    // 保证爬墙模式与地面行走使用同一份输入语义。
    CachedMoveInputIntent.X = FMath::Clamp(MovementVector.Y, -1.0f, 1.0f);
    CachedMoveInputIntent.Y = FMath::Clamp(MovementVector.X, -1.0f, 1.0f);

    // 转发 Lua 表现层（多足步足动画、身体摆动等；不参与移动计算）
    OnInputMove(CachedMoveInputIntent.X, CachedMoveInputIntent.Y);
}

void AZombieCrawlerCharacter::Input_Look(const FInputActionValue& Value)
{
    const FVector2D LookAxisVector = Value.Get<FVector2D>();

    // 相机世界坐标方案（设计决策）：镜头旋转始终跟随 Controller，
    // 爬墙时仅网格体姿态贴墙，镜头不做翻滚；移动输入由运动模式投影到墙面切线平面。
    AddControllerYawInput(LookAxisVector.X);
    AddControllerPitchInput(LookAxisVector.Y);

    // 转发 Lua 表现层（热成像视野微动等）
    OnInputLook(LookAxisVector.X, LookAxisVector.Y);
}

void AZombieCrawlerCharacter::Input_Attack()
{
    // C++ 极薄：技能激活请求（GAS）与第一人称爪击表现全部在 Lua 层处理
    OnInputAttack();
}

void AZombieCrawlerCharacter::Input_Jump()
{
    // 与官方 MoverExamplesCharacter 一致的跳跃输入建模：
    //  - 持续按压标志：供动画/蓄力逻辑读取；
    //  - 刚按下瞬时标志：由输入指令带入模拟，被消费后清零（避免一帧按住重复触发）。
    if (!bIsJumpPressed)
    {
        bIsJumpJustPressed = true;
    }
    bIsJumpPressed = true;

    // 转发 Lua（本地跳跃表现；蹬墙弹射的权威位移由爬墙模式在模拟内消费跳跃标志处理）
    OnInputJump();
}

void AZombieCrawlerCharacter::Input_JumpReleased()
{
    bIsJumpPressed = false;
    // 说明：bIsJumpJustPressed 不在此处清零——即使玩家在两帧模拟之间完成"极短点按"，
    // 也能被下一帧输入生产捕捉到；瞬时标志由 OnProduceInput 消费后清零。
}

void AZombieCrawlerCharacter::HandleMovementModeChanged(const FName& PreviousModeName, const FName& NewModeName)
{
    // 仅关注"进出爬墙模式"的边沿变化，其余模式切换不打扰 Lua 表现层
    const bool bWasClimbing = (PreviousModeName == CrawlerModeNames::Climbing);
    const bool bIsClimbingNow = (NewModeName == CrawlerModeNames::Climbing);

    if (!bWasClimbing && bIsClimbingNow)
    {
        OnClimbStateChanged(true);
    }
    else if (bWasClimbing && !bIsClimbingNow)
    {
        OnClimbStateChanged(false);
    }
}
