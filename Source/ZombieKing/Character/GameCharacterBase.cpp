#include "GameCharacterBase.h"
#include "DefaultMovementSet/CharacterMoverComponent.h"
#include "CombatComponent.h"
#include "HealthComponent.h"
#include "GamePlayerState.h"
#include "AbilitySystemComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Controller.h"
#include "Engine/CollisionProfile.h"

AGameCharacterBase::AGameCharacterBase(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 1. 大厂严禁在基类默认启用 Tick，全面拥抱组件事件/委托驱动
    PrimaryActorTick.bCanEverTick = false;
    PrimaryActorTick.bStartWithTickEnabled = false;

    // 2. 根碰撞体设置（Mover 框架不强制限定胶囊体，此处以基础胶囊体初始化作为示例）
    UCapsuleComponent* CapsuleComp = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCapsule"));
    CapsuleComp->InitCapsuleSize(40.0f, 90.0f);
    // UE5.7：碰撞预设统一走 UCollisionProfile（Pawn 预设自带 Pawn 通道响应，供 Mover 位移扫掠使用）
    CapsuleComp->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
    CapsuleComp->CanCharacterStepUpOn = ECB_No;
    CapsuleComp->SetShouldUpdatePhysicsVolume(true);
    SetRootComponent(CapsuleComp);

    // 3. 真第一人称网格体（同一套 Mesh）
    // 注意：网格体不参与任何碰撞查询——位移与阻挡完全由根胶囊 + Mover 模拟管线负责，
    // 否则爬墙时一发线检测会命中自己的骨骼网格导致吸附判定异常。
    CharacterMesh = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("CharacterMesh"));
    CharacterMesh->SetupAttachment(RootComponent);
    CharacterMesh->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
    CharacterMesh->bCastDynamicShadow = true;
    CharacterMesh->CastShadow = true;
    // 关键：即使本地客户端隐藏了头部骨骼，影子依然必须完整投射！
    CharacterMesh->bCastHiddenShadow = true;

    // 4. 第一人称相机与吸震弹簧臂装配
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    // 绑定至头部骨骼，若为多足爬墙怪，可以在子类中重置绑定点
    CameraBoom->SetupAttachment(CharacterMesh);
    CameraBoom->TargetArmLength = 0.0f; // 0臂长使其精确位于挂接点
    CameraBoom->bUsePawnControlRotation = true; // 视角跟随 Controller 旋转（爬墙时代替方案：镜头保持世界坐标不翻转）
    CameraBoom->bDoCollisionTest = false; // 真第一人称内部相机禁止胶囊体探头碰撞

    FirstPersonCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FirstPersonCamera"));
    FirstPersonCamera->SetupAttachment(CameraBoom);
    FirstPersonCamera->bUsePawnControlRotation = false; // 由 SpringArm 统一接收旋转

    // 5. 挂载 Mover 运动组件
    // 使用引擎 DefaultMovementSet 的 UCharacterMoverComponent：
    //  - 自带 Walking / Falling / Flying 三种默认运动模式与落地检测；
    //  - 自带跳跃输入处理（由 bIsJumpJustPressed 驱动，且有 IsOnGround 门控，
    //    爬墙模式下不会误触发引擎默认跳跃）；
    //  - 子类（多足生化角色）可通过 SetDefaultSubobjectClass 替换为其 Mover 子类，
    //    在组件构造函数里追加自定义运动模式与全局过渡（官方模式）。
    MoverComponent = CreateDefaultSubobject<UCharacterMoverComponent>(TEXT("MoverComponent"));

    // 6. 挂载解耦的战斗与生命组件
    CombatComponent = CreateDefaultSubobject<UCombatComponent>(TEXT("CombatComponent"));
    HealthComponent = CreateDefaultSubobject<UHealthComponent>(TEXT("HealthComponent"));

    // 7. 网络同步基础配置
    bReplicates = true;
    // 使用 Mover 框架时，彻底关闭原生冗余的 Actor 移动同步，全部交由 Mover 内部快照管线
    SetReplicatingMovement(false);

    // 默认配置
    HeadBoneName = FName("head");
    LuaScriptPath = TEXT("Lua.Core.Character.GameCharacterBase");
}

FString AGameCharacterBase::GetModuleName_Implementation() const
{
    // 如果子类在配置中指定了具体的 Lua 脚本模块，则优先动态加载并绑定该脚本
    return LuaScriptPath;
}

// =============================================================================
//  Mover 输入生产者管线
// =============================================================================

void AGameCharacterBase::ProduceInput_Implementation(int32 SimTimeMs, FMoverInputCmdContext& InputCmdResult)
{
    // 入口保持极薄：具体填充逻辑放在可被 C++ 子类重写的 OnProduceInput 中，
    // 与官方 MoverExamplesCharacter 的范式保持一致。
    OnProduceInput(static_cast<float>(SimTimeMs), InputCmdResult);
}

void AGameCharacterBase::OnProduceInput(float DeltaMs, FMoverInputCmdContext& OutInputCmd)
{
    // 输入指令使用引擎默认的角色输入数据块 FCharacterDefaultInputs：
    //  - 移动意图（DirectionalIntent，世界空间，模长 0..1）
    //  - 控制旋转（相机朝向，随输入指令复制到服务器并参与重模拟）
    //  - 跳跃瞬时/持续标志（引擎 UCharacterMoverComponent 与自定义爬墙模式分别消费）
    FCharacterDefaultInputs& CharacterInputs = OutInputCmd.InputCollection.FindOrAddMutableDataByType<FCharacterDefaultInputs>();

    AController* Ctrl = GetController();
    if (!Ctrl)
    {
        // 本地无控制器（服务器上未 Possess 的 Pawn 等）：保持默认空输入即可。
        // 远端模拟代理不会执行输入生产，将沿用最后一份复制过来的输入指令。
        return;
    }

    // 1. 控制旋转：本地玩家取自 PlayerController 的当前相机旋转；
    //    AI 控制器没有相机概念，回退为角色当前朝向（保证生化 AI 也能正常移动/吸附）。
    if (const APlayerController* PC = Cast<APlayerController>(Ctrl))
    {
        CharacterInputs.ControlRotation = PC->GetControlRotation();
    }
    else
    {
        CharacterInputs.ControlRotation = GetActorRotation();
    }

    // 2. 移动意图：把缓存的本地轴输入（X=前后, Y=左右）按控制旋转换算为世界空间方向。
    //    使用 DirectionalIntent 而非绝对速度，交由各运动模式（行走/爬墙）自行解释最大速度。
    const FVector LocalIntent = CachedMoveInputIntent;
    if (!LocalIntent.IsNearlyZero())
    {
        // 注意：FRotator::RotateVector 返回 FVector；
        // 不可使用 FRotationMatrix::TransformVector（LWC 下返回 TVector4，无法取模长/限幅）
        const FVector WorldIntent = CharacterInputs.ControlRotation.RotateVector(LocalIntent).GetClampedToMaxSize(1.0f);
        CharacterInputs.SetMoveInput(EMoveInputType::DirectionalIntent, WorldIntent);
    }
    else
    {
        // 零向量 = 明确表达"无移动意图"（爬墙模式下即表现为吸附静止）
        CharacterInputs.SetMoveInput(EMoveInputType::DirectionalIntent, FVector::ZeroVector);
    }

    // 3. 跳跃标志：持续按压与瞬时按下分开传，供不同状态消费
    CharacterInputs.bIsJumpPressed = bIsJumpPressed;
    CharacterInputs.bIsJumpJustPressed = bIsJumpJustPressed;

    // 4. 消费瞬时输入：与官方 MoverExamplesCharacter 一致，
    //    避免一次按键在多个模拟帧里被重复判定（例如连续多次蹬墙弹射）。
    bIsJumpJustPressed = false;
}

// =============================================================================
//  生命周期
// =============================================================================

void AGameCharacterBase::BeginPlay()
{
    Super::BeginPlay();

    // 根据端类型配置网格体与视线遮挡
    SetupTrueFirstPersonMesh();
}

void AGameCharacterBase::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);

    // 仅在服务器端：当 Controller 控制该 Pawn 时，PlayerState 已就绪，初始化 GAS
    SetupAbilitySystemContext();

    // 刷新真第一人称视线状态
    SetupTrueFirstPersonMesh();
}

void AGameCharacterBase::OnRep_PlayerState()
{
    Super::OnRep_PlayerState();

    // 仅在客户端：接收到 PlayerState 复制时，初始化本端的 GAS 上下文
    SetupAbilitySystemContext();
}

void AGameCharacterBase::OnRep_Controller()
{
    Super::OnRep_Controller();

    // 控制权切换（如本地回放或幽灵观察者切换），刷新头部骨骼可见性
    SetupTrueFirstPersonMesh();
}

// =============================================================================
//  真第一人称表现
// =============================================================================

void AGameCharacterBase::SetupTrueFirstPersonMesh()
{
    if (!CharacterMesh) return;

    // 判定是否为"由本地玩家直接控制的客户端实体"
    const bool bIsLocalFPS = IsLocallyControlled();

    if (bIsLocalFPS)
    {
        // 【真第一人称核心技巧】：
        // 对本地眼睛隐藏头部骨骼，防止移动、呼吸动画时自己看到自己的脸部穿模
        if (HeadBoneName != NAME_None && CharacterMesh->GetBoneIndex(HeadBoneName) != INDEX_NONE)
        {
            CharacterMesh->HideBoneByName(HeadBoneName, PBO_None);
        }

        // 关键设置：虽然骨骼被隐藏，但依然投射影子，保持自身低头看脚看影子时的沉浸感
        CharacterMesh->bCastHiddenShadow = true;
        // 允许头部微动态影响子骨骼阴影计算
        CharacterMesh->MarkRenderStateDirty();
    }
    else
    {
        // 远端模拟代理（其他玩家眼中的此角色）或 Dedicated Server：完整显示所有骨骼
        if (HeadBoneName != NAME_None)
        {
            CharacterMesh->UnHideBoneByName(HeadBoneName);
        }
    }
}

// =============================================================================
//  GAS 上下文
// =============================================================================

void AGameCharacterBase::SetupAbilitySystemContext()
{
    AGamePlayerState* PS = GetPlayerState<AGamePlayerState>();
    if (!PS) return;

    UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent();
    if (ASC)
    {
        // 工业级 GAS 规范：长期存活的 PlayerState 为 OwnerActor，短生命周期的 Pawn 为 AvatarActor
        ASC->InitAbilityActorInfo(PS, this);

        // 通知解耦的生命管理组件与 ASC 完成事件订阅与属性委托绑定
        if (HealthComponent)
        {
            HealthComponent->InitializeWithAbilitySystem(ASC);
        }
    }
}

UAbilitySystemComponent* AGameCharacterBase::GetAbilitySystemComponent() const
{
    AGamePlayerState* PS = GetPlayerState<AGamePlayerState>();
    return PS ? PS->GetAbilitySystemComponent() : nullptr;
}

void AGameCharacterBase::SetGenericTeamId(const FGenericTeamId& InTeamID)
{
    if (AGamePlayerState* PS = GetPlayerState<AGamePlayerState>())
    {
        PS->SetGenericTeamId(InTeamID);
    }
}

FGenericTeamId AGameCharacterBase::GetGenericTeamId() const
{
    AGamePlayerState* PS = GetPlayerState<AGamePlayerState>();
    return PS ? PS->GetGenericTeamId() : FGenericTeamId::NoTeam;
}

void AGameCharacterBase::UnPossessed()
{
    // 感染变异、切换角色或死亡离身时，重置 AvatarActor，防止 GAS 回调悬空野指针
    if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
    {
        ASC->SetAvatarActor(nullptr);
    }
    Super::UnPossessed();
}
