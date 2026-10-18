#include "GameCharacterBase.h"
#include "DefaultMovementSet/CharacterMoverComponent.h"
#include "CombatComponent.h"
#include "HealthComponent.h"
#include "GameWeaponComponent.h"          // 武器组件（装备槽/弹药/换弹/切枪/开火机制的权威）
#include "GameAbilitySystemComponent.h"   // 命中/击杀确认事件（ASC → 角色桥接）+ 进化 GE 施加
#include "GameEffectTags.h"               // GameEffectTags::Data_ZombieMaxHealthDelta（进化 SetByCaller）
#include "GameplayEffect.h"               // FGameplayEffectSpec（SetByCaller 需要完整类型）
#include "GamePlayerState.h"
#include "AbilitySystemComponent.h"
#include "Net/UnrealNetwork.h"            // DOREPLIFETIME（硬直状态复制）
#include "Camera/CameraComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/BoxComponent.h"      // UBoxComponent：宽扁体型的盒体根碰撞（SetBoxCollisionRoot / ApplyBodyProfile）
#include "GameCharacterBodyProfile.h"     // 体型档案数据资产：高矮胖瘦尺寸来源
#include "GameCharacterDefinition.h"      // 角色定义数据资产：体型/阵营/技能集/初始属性来源
#include "GameAbilitySet.h"               // 技能集数据资产：展平为技能列表供 PlayerState 授予
#include "GameInputConfig.h"              // 技能输入配置（输入动作 → 技能标签）
#include "GameGameMode.h"                 // 对局 GameMode：死亡上报入口（ReportPlayerDeath）
#include "InputAction.h"                  // UInputAction 完整类型（BindAction 形参需要）
#include "DefaultMovementSet/Settings/CommonLegacyMovementSettings.h" // Mover 共享移动设置（行走速度/跳跃初速）
#include "ZombieKing.h"                   // LogZombieKing 项目日志分类（体型配置告警）
#include "GameFramework/PlayerController.h"
#include "GameFramework/Controller.h"
#include "Engine/CollisionProfile.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"

AGameCharacterBase::AGameCharacterBase(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 1. 大厂严禁在基类默认启用 Tick，全面拥抱组件事件/委托驱动
    PrimaryActorTick.bCanEverTick = false;
    PrimaryActorTick.bStartWithTickEnabled = false;

    // 2. 根碰撞体设置（Mover 框架不强制限定胶囊体，此处以基础胶囊体初始化作为示例）
    //    注意：这里只是"类默认值"，蓝图子类在组件 Details 中修改的尺寸会覆盖此默认值
    CollisionCapsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CollisionCapsule"));
    CollisionCapsule->InitCapsuleSize(40.0f, 90.0f);
    // UE5.7：碰撞预设统一走 UCollisionProfile（Pawn 预设自带 Pawn 通道响应，供 Mover 位移扫掠使用）
    CollisionCapsule->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
    CollisionCapsule->CanCharacterStepUpOn = ECB_No;
    CollisionCapsule->SetShouldUpdatePhysicsVolume(true);
    SetRootComponent(CollisionCapsule);

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

    // 6.1 武器组件（武器系统的服务器权威）：
    //     所有角色都挂载 —— 人类在蓝图里配武器槽，生化可以留空（没有武器则开火被安全拒绝）。
    //     这样"能不能开火"永远走同一套机制，不必为不同阵营各写一套武器逻辑。
    WeaponComponent = CreateDefaultSubobject<UGameWeaponComponent>(TEXT("WeaponComponent"));

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
    //
    // -------------------------------------------------------------------------
    // 受击硬直：**在输入侧**削减移动意图（本项目已拍板的落点）
    // -------------------------------------------------------------------------
    // 硬直期间把移动意图置零（并禁跳）而不是去改角色速度，原因：
    //  ① 输入指令会随移动管线**复制到其它端并参与重模拟**，因此"硬直期间不能动"在各端表现完全一致，
    //     不会出现"本地能走、服务器拉回"的预测抖动；
    //  ② 硬直的时长来自 CombatComponent 的部位判定（服务器权威），复制给客户端后两端各自倒计时，
    //     客户端因此能在本地立刻削减意图（无需等服务器指令），手感上"被打中就动不了"是即时的；
    //  ③ 对爬墙角色同样有效：爬墙模式下"零移动意图"即表现为吸附静止，硬直期间无法爬动。
    //     **设计契约（用户拍板，2026 爬墙专项确认）：硬直不会把爬墙者从墙上打落** ——
    //     可爬墙的生化角色被攻击硬直时**贴墙不掉**：Climbing 模式默认 GripStrength=1（完全吸附），
    //     零意图下切向速度与重力滑落分量皆为 0，角色钉死在原地（含禁跳，蹬墙弹射同样被堵死）。
    //     刻意**不做**"硬直强制脱离墙面"：唯一会掉墙的情形是物理上失去表面（墙被摧毁/爬出边缘），
    //     与硬直无关；内容侧若给某类幽灵配 GripStrength<1（差异化"缓降怪"），其硬直下滑属设计选择，
    //     不在机制层锁死。后人在此加"硬直切模式/击落"逻辑前，先确认玩法设计是否变更。
    // 控制旋转仍照常写入（否则服务器侧视角与客户端不一致），受影响的只有移动与跳跃意图。
    const bool bStaggered = IsStaggered();

    const FVector LocalIntent = bStaggered ? FVector::ZeroVector : CachedMoveInputIntent;
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

    // 3. 面向意图：真第一人称下身体朝向跟随相机（地面行走始终朝视线方向推进）。
    //    AI 控制器的 ControlRotation 已回退为角色自身朝向，因此不会产生额外转向；
    //    爬墙姿态（Up = 墙面法线）由爬墙运动模式在模拟内独立解算，与此互不冲突。
    CharacterInputs.OrientationIntent = CharacterInputs.ControlRotation.Vector().GetSafeNormal();

    // 4. 跳跃标志：持续按压与瞬时按下分开传，供不同状态消费
    //    硬直期间一律禁止跳跃（否则"被打中还能跳"会让硬直形同虚设）
    CharacterInputs.bIsJumpPressed = bStaggered ? false : bIsJumpPressed;
    CharacterInputs.bIsJumpJustPressed = bStaggered ? false : bIsJumpJustPressed;

    // 5. 消费瞬时输入：与官方 MoverExamplesCharacter 一致，
    //    避免一次按键在多个模拟帧里被重复判定（例如连续多次蹬墙弹射）。
    bIsJumpJustPressed = false;
}

// =============================================================================
//  通用输入管线（Enhanced Input → 缓存 → 输入指令）
//  设计说明：移动/视角/跳跃/攻击为所有角色共用的基础输入通道，绑定与缓存统一在基类完成
//  （与 Epic 官方 Mover 参考实现 MoverExamplesCharacter 的组织方式一致）；
//  子类需要额外按键（技能/换弹/蹲伏等）时，重写 SetupPlayerInputComponent 并在 Super 后追加。
// =============================================================================

void AGameCharacterBase::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
    Super::SetupPlayerInputComponent(PlayerInputComponent);

    // 自诊断：未在蓝图 Class Defaults 指定输入资产的通道会被静默跳过；
    // 这里显式告警，便于第一时间定位"某个按键没反应 = 资产没配"的问题。
    if (!DefaultMappingContext)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未指定 DefaultMappingContext：IMC 未激活，全部 Enhanced Input 按键都不会生效（请在蓝图 Class Defaults → Input 分类下配置）"), *GetName());
    }
    if (!ActionMove)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未指定 ActionMove：WASD 移动输入绑定被跳过（请在蓝图 Class Defaults → Input 分类下配置）"), *GetName());
    }
    if (!ActionLook)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未指定 ActionLook：视角输入绑定被跳过"), *GetName());
    }
    if (!ActionAttack)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未指定 ActionAttack：攻击输入绑定被跳过"), *GetName());
    }
    if (!ActionJump)
    {
        UE_LOG(LogTemp, Warning, TEXT("[%s] 未指定 ActionJump：跳跃输入绑定被跳过（请在蓝图 Class Defaults → Input 分类下配置）"), *GetName());
    }

    // 1. 激活 Enhanced Input 映射上下文（各角色蓝图在 Class Defaults 指定自己的 IMC）
    if (const APlayerController* PC = Cast<APlayerController>(GetController()))
    {
        if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
        {
            if (DefaultMappingContext)
            {
                Subsystem->AddMappingContext(DefaultMappingContext, 0);
            }
        }
    }

    // 2. 绑定通用输入动作：只做"缓存 + 转发 Lua"，移动计算全部交给 Mover 模拟
    UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
    if (!EnhancedInput)
    {
        return;
    }

    if (ActionMove)
    {
        EnhancedInput->BindAction(ActionMove, ETriggerEvent::Triggered, this, &AGameCharacterBase::Input_Move);

        // 关键：必须同时绑定 Completed 事件（官方 Mover 参考实现 MoverExamplesCharacter 的标准做法）。
        // 原因：Enhanced Input 在"最后一个按键松开"时只保证派发 Completed，不保证再派发一遍零值 Triggered；
        // 而 Mover 的 DirectionalIntent 语义会按输入指令持续全速移动（不像传统 CMC 有地面摩擦自动停车），
        // 不显式清零缓存就会出现"松手后角色一直朝一个方向漂移"。
        EnhancedInput->BindAction(ActionMove, ETriggerEvent::Completed, this, &AGameCharacterBase::Input_MoveCompleted);
    }
    if (ActionLook)
    {
        EnhancedInput->BindAction(ActionLook, ETriggerEvent::Triggered, this, &AGameCharacterBase::Input_Look);
    }
    if (ActionAttack)
    {
        EnhancedInput->BindAction(ActionAttack, ETriggerEvent::Started, this, &AGameCharacterBase::Input_Attack);
    }
    if (ActionFire)
    {
        // 开火必须绑"按下 + 松开"两事件：按住连发、松手停火都依赖松手信号
        EnhancedInput->BindAction(ActionFire, ETriggerEvent::Started, this, &AGameCharacterBase::Input_FireStart);
        EnhancedInput->BindAction(ActionFire, ETriggerEvent::Completed, this, &AGameCharacterBase::Input_FireRelease);
    }
    if (ActionJump)
    {
        EnhancedInput->BindAction(ActionJump, ETriggerEvent::Started, this, &AGameCharacterBase::Input_Jump);
        EnhancedInput->BindAction(ActionJump, ETriggerEvent::Completed, this, &AGameCharacterBase::Input_JumpReleased);
    }

    // 武器相关输入（可选通道：没配就不绑定，也不告警 —— 生化幽灵等"没有枪"的角色属正常情况）
    if (ActionReload)
    {
        EnhancedInput->BindAction(ActionReload, ETriggerEvent::Started, this, &AGameCharacterBase::Input_Reload);
    }
    if (ActionNextWeapon)
    {
        EnhancedInput->BindAction(ActionNextWeapon, ETriggerEvent::Started, this, &AGameCharacterBase::Input_SwitchWeapon);
    }

    // 3. 绑定技能输入：把"输入动作 → 技能标签"映射挂上去（配置来自角色定义，改按键不必重编译）
    BindAbilityInputActions(EnhancedInput);
}

void AGameCharacterBase::Input_Move(const FInputActionValue& Value)
{
    // 增强输入 IA 轴约定（与引擎模板一致）：X = 左/右，Y = 前/后
    const FVector2D MovementVector = Value.Get<FVector2D>();

    // 缓存为"本地意图"（X = 前后, Y = 左右，取值 -1..1）；
    // 世界空间换算（按相机朝向）统一在 OnProduceInput 中完成，随输入指令进入预测管线。
    CachedMoveInputIntent.X = FMath::Clamp(MovementVector.Y, -1.0f, 1.0f);
    CachedMoveInputIntent.Y = FMath::Clamp(MovementVector.X, -1.0f, 1.0f);

    // 转发 Lua 表现层（步足/腿部动画参数等，不参与移动计算）
    OnInputMove(CachedMoveInputIntent.X, CachedMoveInputIntent.Y);
}

void AGameCharacterBase::Input_MoveCompleted(const FInputActionValue& Value)
{
    // 松开移动键（所有按键已释放）：显式清零缓存意图，
    // 使后续输入指令表达"无移动意图"，MovementMode 随即减速停车。
    CachedMoveInputIntent = FVector::ZeroVector;

    // 转发 Lua 表现层复位（步足动画回到待机参数等）
    OnInputMove(0.0f, 0.0f);
}

void AGameCharacterBase::Input_Look(const FInputActionValue& Value)
{
    const FVector2D LookAxisVector = Value.Get<FVector2D>();

    // 相机世界坐标方案：镜头旋转始终跟随 Controller（爬墙时仅网格体贴墙，镜头不翻转）
    AddControllerYawInput(LookAxisVector.X);
    AddControllerPitchInput(LookAxisVector.Y);

    // 转发 Lua 表现层（热成像视野微动等）
    OnInputLook(LookAxisVector.X, LookAxisVector.Y);
}

void AGameCharacterBase::Input_Attack()
{
    // C++ 极薄：技能激活请求（GAS）与第一人称攻击表现全部在 Lua 层处理
    OnInputAttack();
}

void AGameCharacterBase::Input_FireStart()
{
    // C++ 极薄：只转发 Lua；「开火（含连发节奏请求）+ 本地即时表现」全部由 Lua 调武器组件完成
    OnInputFireStart();
}

void AGameCharacterBase::Input_FireRelease()
{
    // 松开：只转发 Lua（由 Lua 调武器组件 StopFire 停火）
    OnInputFireRelease();
}

void AGameCharacterBase::Input_Jump()
{
    // 与官方 MoverExamplesCharacter 一致的跳跃输入建模：
    //  - 持续按压标志：供动画/蓄力逻辑读取；
    //  - 刚按下瞬时标志：由输入指令带入模拟，被消费后清零（避免一帧按住重复触发）。
    if (!bIsJumpPressed)
    {
        bIsJumpJustPressed = true;
    }
    bIsJumpPressed = true;

    // 转发 Lua（本地跳跃表现；蹬墙弹射/地面起跳的权威位移由运动模式在模拟内消费跳跃标志处理）
    OnInputJump();
}

void AGameCharacterBase::Input_JumpReleased()
{
    bIsJumpPressed = false;
    // 说明：bIsJumpJustPressed 不在此处清零——即使玩家在两帧模拟之间完成"极短点按"，
    // 也能被下一帧输入生产捕捉到；瞬时标志由 OnProduceInput 消费后清零。
}

void AGameCharacterBase::Input_Reload()
{
    // C++ 极薄：只把按键事件转发给 Lua；换弹请求（含服务器权威校验）由 Lua 调武器组件执行
    OnInputReload();
}

void AGameCharacterBase::Input_SwitchWeapon()
{
    // 同上：切枪的规则（允许切到哪一把、是否打断换弹）由 Lua 决定，C++ 只转发
    OnInputSwitchWeapon();
}

// =============================================================================
//  技能输入（输入动作 → 技能标签 → 按标签激活技能）
// =============================================================================

void AGameCharacterBase::BindAbilityInputActions(UEnhancedInputComponent* EnhancedInput)
{
    if (!EnhancedInput)
    {
        return;
    }

    // 输入配置来自角色定义（大厂做法：输入映射是内容资产，换按键/加技能键都无需重编译 C++）
    const UGameInputConfig* InputConfig = CharacterDefinition ? CharacterDefinition->InputConfig : nullptr;
    if (!InputConfig)
    {
        return;
    }

    for (const FGameInputAction& InputBinding : InputConfig->InputActions)
    {
        // 跳过未填完整的配置项（避免蓝图里留半成品条目导致绑定无效或崩溃）
        if (!InputBinding.InputAction || !InputBinding.InputTag.IsValid())
        {
            continue;
        }

        // 关键：把"输入标签"作为附加载荷参数直接绑进回调
        //（EnhancedInputComponent::BindAction 支持 VarTypes... 附加参数），
        // 因此无论有多少种技能输入，都只需要这一个回调函数，无需逐个新增。
        EnhancedInput->BindAction(InputBinding.InputAction, ETriggerEvent::Started,
            this, &AGameCharacterBase::Input_AbilityInputTagPressed, InputBinding.InputTag);
    }
}

void AGameCharacterBase::Input_AbilityInputTagPressed(const FInputActionValue& Value, FGameplayTag InputTag)
{
    // 权威侧：交给 GAS 按标签批量激活（是否本地预测/上服务器由各技能自身的 NetExecutionPolicy 决定）。
    // 这里刻意不判 HasAuthority —— 输入发生在客户端，激活请求必须能由客户端发起。
    if (UAbilitySystemComponent* AbilitySystemComponent = GetAbilitySystemComponent())
    {
        FGameplayTagContainer InputTagContainer;
        InputTagContainer.AddTag(InputTag);
        AbilitySystemComponent->TryActivateAbilitiesByTag(InputTagContainer, /*bAllowRemoteActivation=*/true);
    }

    // 表现侧：转发 Lua（按键反馈、音效、准星变化等），保持"C++ 极薄、表现下沉 Lua"
    OnInputAbility(InputTag);
}

// =============================================================================
//  生命周期
// =============================================================================

void AGameCharacterBase::BeginPlay()
{
    Super::BeginPlay();

    // 缓存蓝图默认骨骼网格体（只做一次）：进化"模型小变化"的还原基准（等级归 0 / 缺配置时回到原模型）
    if (CharacterMesh && !DefaultCharacterSkeletalMesh.IsValid())
    {
        DefaultCharacterSkeletalMesh = CharacterMesh->GetSkeletalMeshAsset();
    }

    // 体型档案在运行期灌入：各端持有同一份 EditDefaultsOnly 资产，计算出的碰撞尺寸天然一致，
    // 因此无需走网络复制（避免"碰撞尺寸各自为政"导致的预测分歧）。
    ApplyBodyProfile();

    // 移动参数（行走速度 / 跳跃高度）灌入 Mover：让"逐类型速度/跳跃差异"真正生效
    ApplyMovementParams();

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

    // 控制权就绪通知（服务端路径）：Lua 据此做"视角复位"等需要控制权的表现
    OnControllerReady();
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

    // 控制权就绪通知（客户端路径）：
    // 客户端 Pawn 不会走 PossessedBy（那是服务端路径），这里才是客户端"我拥有这个角色了"的可靠时机，
    // 用于把死亡时切给击杀者的镜头切回自己（见 GameCharacterBase.lua 的 OnControllerReady）。
    OnControllerReady();
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

        // 服务器权威：用"初始化属性"GameplayEffect 灌入初始生命等数值。
        // 该 GE 来自角色定义（每种角色类型一份资产 —— "每只生化幽灵血量不同"由资产决定）。
        // 必须在绑定生命委托之前施加，否则初始化过程中的数值变化会被当成一次"生命变化"误触发表现。
        PS->ApplyInitialAttributeSetEffect(CharacterDefinition ? CharacterDefinition->InitialAttributeSetEffect : nullptr);

        // 服务器权威：授予角色定义中技能集内的技能（"每只生化幽灵技能不同"由 CharacterDefinition 决定）。
        // 函数内部会先清除上一次授予的技能，保证换人/变异后技能集与当前角色严格一致。
        if (HasAuthority())
        {
            // 把多套技能集展平为一份技能列表：PlayerState 只认列表，接口保持简单
            TArray<TSubclassOf<UGameplayAbility>> AbilitiesToGrant;
            if (CharacterDefinition)
            {
                for (const TObjectPtr<UGameAbilitySet>& AbilitySet : CharacterDefinition->AbilitySets)
                {
                    if (AbilitySet)
                    {
                        AbilitiesToGrant.Append(AbilitySet->GrantedAbilities);
                    }
                }
            }
            PS->GrantCharacterAbilities(AbilitiesToGrant);
        }

        // 通知解耦的生命管理组件与 ASC 完成事件订阅与属性委托绑定
        if (HealthComponent)
        {
            HealthComponent->InitializeWithAbilitySystem(ASC);

            // 死亡事件去重绑定：PossessedBy（服务器）与 OnRep_PlayerState（客户端）都会走到这里，
            // 不去重会导致同一次死亡被重复转发（复活流程被触发两次、死亡演出重复播放）。
            HealthComponent->OnDeath.RemoveDynamic(this, &AGameCharacterBase::HandleHealthDeath);
            HealthComponent->OnDeath.AddDynamic(this, &AGameCharacterBase::HandleHealthDeath);
        }

        // 战斗组件：订阅 ASC 生命变化（部位判定 / 受击硬直 / 命中校验），
        // 并把其受击事件桥接到角色的 OnDamaged → Lua 受击表现。
        if (CombatComponent)
        {
            CombatComponent->InitializeWithAbilitySystem(ASC);
            // 先移除再绑定：PossessedBy（服务器）与 OnRep_PlayerState（客户端）都会走到这里，
            // 不去重会导致同一次受击被重复转发（表现抖动、事件重复）。
            CombatComponent->OnDamagedReceived.RemoveDynamic(this, &AGameCharacterBase::HandleCombatDamaged);
            CombatComponent->OnDamagedReceived.AddDynamic(this, &AGameCharacterBase::HandleCombatDamaged);

            // 受击硬直：组件算出时长后由角色接手"写入复制字段 + 起本地倒计时"（见 HandleCombatStaggered）
            CombatComponent->OnStaggered.RemoveDynamic(this, &AGameCharacterBase::HandleCombatStaggered);
            CombatComponent->OnStaggered.AddDynamic(this, &AGameCharacterBase::HandleCombatStaggered);
        }

        // 命中/击杀确认：把 ASC 的"定向确认"事件桥接到角色自身的委托（供 Lua / HUD 订阅）。
        // 说明：ASC 托管在长生命周期的 PlayerState 上，动态委托对 UObject 是弱引用 ——
        //       角色销毁后绑定会自动失效，因此这里无需在 UnPossessed 里手工解绑（仅需去重）。
        if (UGameAbilitySystemComponent* GameASC = Cast<UGameAbilitySystemComponent>(ASC))
        {
            GameASC->OnHitConfirmed.RemoveDynamic(this, &AGameCharacterBase::HandleASC_HitConfirmed);
            GameASC->OnHitConfirmed.AddDynamic(this, &AGameCharacterBase::HandleASC_HitConfirmed);

            GameASC->OnKillConfirmed.RemoveDynamic(this, &AGameCharacterBase::HandleASC_KillConfirmed);
            GameASC->OnKillConfirmed.AddDynamic(this, &AGameCharacterBase::HandleASC_KillConfirmed);
        }

        // -------------------------------------------------------------------------
        // 进化等级接线（CF 式 6 级轨）
        // -------------------------------------------------------------------------
        // 订阅等级变化：升级瞬间各端刷新"体型缩放 + 模型替换"（动态委托弱引用，角色销毁自动失效，仅需去重）。
        PS->OnZombieLevelChanged.RemoveDynamic(this, &AGameCharacterBase::HandleZombieLevelChanged);
        PS->OnZombieLevelChanged.AddDynamic(this, &AGameCharacterBase::HandleZombieLevelChanged);

        // 新 Avatar 的等级对齐：复活/变身生成的角色是"满血基准属性"，
        // 按 PlayerState **当前**等级一次性补算属性增量并应用体型/模型 ——
        // 于是"Avatar 形态恒等于权威等级状态"：
        // 当前规则下被枪杀复活的幽灵等级已被 Lua 重置为 1（本路径自然只做 1 级形态、不补增量）；
        // 若将来规则改为"跨死亡保留等级"，Lua 不重置即可，机制层无需改动。
        AppliedEvolutionLevel = 1; // 新 Avatar 从"1 级基准"起算（初始化 GE 灌入的就是 1 级血量）
        const int32 CurrentLevel = PS->GetZombieLevel();
        if (CurrentLevel > 1 && HasAuthority())
        {
            ApplyEvolutionAttributeDelta(1, CurrentLevel);
            AppliedEvolutionLevel = CurrentLevel;
        }
        if (CurrentLevel > 0)
        {
            // 各端都要摆体型/模型：Possess 瞬间不会有"变化事件"（等级没变），必须主动应用一次
            ApplyZombieLevelVisuals(CurrentLevel);
        }
    }
}

void AGameCharacterBase::HandleCombatDamaged(float DamageAmount, FName HitBone, AActor* InstigatorActor, bool bMeleeHit)
{
    // 刷新"最近一次受击是否为头部"：死亡事件本身不带命中细节，
    // 而击杀播报需要"是否爆头击杀"，因此在受击时把部位结论缓存下来（部位判定复用 CombatComponent 的唯一规则源）。
    if (CombatComponent && !HitBone.IsNone())
    {
        bLastHitWasHeadshot = (CombatComponent->DetermineHitZone(HitBone) == EHitZone::Head);
    }

    // 刷新"最近一次受击是否近战"：供 Lua 死亡规则实现 CF 式"刀杀禁令"（服务器侧读取）。
    // 结论由 CombatComponent 从伤害规格的动态资产标签判定（近战链路施加时自动携带，内容侧零配置）。
    bLastHitWasMelee = bMeleeHit;

    // 桥接：组件事件 → 角色的 BlueprintNativeEvent（Lua 侧 OnDamaged_Implementation 接管表现）
    OnDamaged(DamageAmount, HitBone, InstigatorActor);
}

// =============================================================================
//  生化幽灵进化（等级落地：血量增量 + 体型缩放 + 模型替换；规则与数值表在 Lua/资产）
// =============================================================================

void AGameCharacterBase::SetZombieLevel(int32 NewLevel)
{
    // 仅服务器：等级与血量增量都是权威状态，客户端无权自置（防"自选等级"作弊）
    if (!HasAuthority())
    {
        return;
    }

    AGamePlayerState* PS = GetPlayerState<AGamePlayerState>();
    if (!PS)
    {
        return;
    }

    // 1) 写 PlayerState 状态（复制 + 服务器本地广播）：
    //    各端角色经 HandleZombieLevelChanged 刷新体型/模型；等级未变时 PS 内部短路、不产生多余事件。
    PS->SetZombieLevel(NewLevel);

    // 2) 服务器施加属性增量（幂等：只补"本 Avatar 尚未应用过"的区间，
    //    防 Lua 重复调用同一等级、以及与复活恢复路径（SetupAbilitySystemContext）双算）。
    const int32 From = FMath::Max(AppliedEvolutionLevel, 1);
    if (NewLevel > From)
    {
        ApplyEvolutionAttributeDelta(From, NewLevel);
        AppliedEvolutionLevel = NewLevel;
    }
}

void AGameCharacterBase::HandleZombieLevelChanged(int32 NewLevel)
{
    // 只落"本地表现"（体型/模型）；属性增量刻意不在这里补 —— 那由服务器 SetZombieLevel
    // 与复活恢复两处显式调用，集中入口防双算。
    ApplyZombieLevelVisuals(NewLevel);
}

void AGameCharacterBase::ApplyZombieLevelVisuals(int32 Level)
{
    // 等级 0（人类形态/回合重置）：还原基准体型与默认模型（幂等，重复调用无副作用）
    const FGameZombieLevelEntry* Entry = nullptr;
    if (CharacterDefinition && Level > 0 && CharacterDefinition->ZombieLevels.IsValidIndex(Level - 1))
    {
        Entry = &CharacterDefinition->ZombieLevels[Level - 1];
    }

    // 体型缩放：整 Actor 统一缩放（胶囊碰撞与网格一起放大 —— 受击判定随体型变化，各端由同一资产推导、天然一致）。
    // 注意：缩放的是"体型档案灌入后的既有尺寸"之上再乘系数，不改碰撞形态（形态仍在构造期决定，项目铁律）。
    const float Scale = Entry ? Entry->BodyScale : 1.0f;
    SetActorScale3D(FVector(Scale));

    // 模型替换："模型小变化"= 同骨架换骨骼网格体（不换类、不换动画蓝图，代价最小）。
    // 缺配置/等级归 0 → 还原 BeginPlay 缓存的默认网格。
    if (CharacterMesh)
    {
        USkeletalMesh* TargetMesh = DefaultCharacterSkeletalMesh.Get();
        if (Entry && !Entry->MeshOverride.IsNull())
        {
            // 升级是低频事件，同步加载可接受（资产通常已随角色进包且在内存驻留）
            TargetMesh = Entry->MeshOverride.LoadSynchronous();
        }
        if (TargetMesh && CharacterMesh->GetSkeletalMeshAsset() != TargetMesh)
        {
            CharacterMesh->SetSkeletalMesh(TargetMesh, /*bForceUpdateAnimation=*/true);
        }
    }
}

void AGameCharacterBase::ApplyEvolutionAttributeDelta(int32 FromLevelExclusive, int32 ToLevel)
{
    if (!HasAuthority() || ToLevel <= FromLevelExclusive)
    {
        return;
    }

    AGamePlayerState* PS = GetPlayerState<AGamePlayerState>();
    UGameAbilitySystemComponent* GameASC = PS ? PS->GetGameAbilitySystemComponent() : nullptr;
    if (!GameASC || !CharacterDefinition)
    {
        return;
    }

    // 累加 (FromLevelExclusive, ToLevel] 区间的生命上限增量
    //（表下标 i = 等级 i+1；等级 L 的增量存在下标 L-1；1 级应配 0，因为 1 级血量=初始化 GE 的基准）。
    float SumDelta = 0.0f;
    for (int32 i = FromLevelExclusive; i < ToLevel; ++i)
    {
        if (CharacterDefinition->ZombieLevels.IsValidIndex(i))
        {
            SumDelta += CharacterDefinition->ZombieLevels[i].MaxHealthDelta;
        }
    }
    if (SumDelta <= 0.0f)
    {
        return; // 只升体型/不加血：合法配置，无需 GE
    }

    if (!CharacterDefinition->ZombieEvolutionEffect)
    {
        // 内容侧漏配：告警并降级（体型/模型照常，血量不涨），绝不崩
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 角色定义未配置 ZombieEvolutionEffect：进化等级 %d→%d 的生命增量（%.0f）不会被施加"
                 "（请建共享进化 GE：MaxHealth/Health 各加 SetByCaller Data.Zombie.MaxHealthDelta）。"),
            *GetName(), FromLevelExclusive, ToLevel, SumDelta);
        return;
    }

    // 走"共享进化 GE + SetByCaller 增量"通道：一份 GE 服务所有幽灵与所有级差（数值全在资产表）。
    FGameplayEffectContextHandle ContextHandle = GameASC->MakeEffectContext();
    ContextHandle.AddSourceObject(this);

    const FGameplayEffectSpecHandle SpecHandle =
        GameASC->MakeOutgoingSpec(CharacterDefinition->ZombieEvolutionEffect, 1.0f, ContextHandle);
    if (!SpecHandle.IsValid())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[%s] 进化 GE 规格生成失败，请检查 ZombieEvolutionEffect 资产。"), *GetName());
        return;
    }

    SpecHandle.Data->SetSetByCallerMagnitude(GameEffectTags::Data_ZombieMaxHealthDelta, SumDelta);
    GameASC->ApplyGameplayEffectSpecToSelf(*SpecHandle.Data.Get());
}

// =============================================================================
//  受击硬直（服务器权威状态 → 复制 → 两端各自本地倒计时）
// =============================================================================

void AGameCharacterBase::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // 只复制"硬直时长"这一个字段：
    //  - 客户端据此起本地倒计时（无需知道服务器时间戳，避免跨机时钟依赖）；
    //  - 服务器自身的倒计时由 HandleCombatStaggered 直接起，不依赖复制。
    DOREPLIFETIME(AGameCharacterBase, ReplicatedStaggerSeconds);

    // 复制"控制者瞄准朝向"：观战相机（第一人称跟随）与远端客户端的瞄准/持枪表现都依赖它。
    // 之所以不由引擎的 RemoteViewPitch 代劳：那只有俯仰，而本项目角色不随控制器偏航旋转，
    // 偏航必须自己复制（见 GameCharacterBase.h 的 GetReplicatedAimRotation 注释）。
    DOREPLIFETIME(AGameCharacterBase, ReplicatedAimRotation);
}

void AGameCharacterBase::PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker)
{
    Super::PreReplication(ChangedPropertyTracker);

    // 仅服务器写入：服务器握有每个玩家控制器最新的控制旋转
    //（由该玩家客户端上报的移动/视角输入在服务器侧被更新，见 AController::ControlRotation）。
    // 这与引擎自身在 APawn::PreReplication 里写 RemoteViewPitch 的做法同源、同权威。
    if (HasAuthority())
    {
        if (const AController* OwningController = GetController())
        {
            ReplicatedAimRotation = OwningController->GetControlRotation();
        }
    }
}

void AGameCharacterBase::HandleCombatStaggered(float Duration, EHitZone HitZone, AActor* InstigatorActor)
{
    // 硬直判定（部位 → 时长）只在服务器有意义：属性变化回调在客户端也会触发，
    // 但客户端不应自行裁定硬直（否则客户端可以"自己给自己免硬直"），故这里双保险判权威端。
    if (!HasAuthority() || Duration <= 0.0f)
    {
        return;
    }

    // 写入复制字段：客户端在 OnRep_StaggerState 里拿到同样的时长并起本地倒计时
    ReplicatedStaggerSeconds = Duration;

    // 服务器本端：立刻起倒计时并广播（服务器不会收到自己的 RepNotify）
    ApplyStaggerLocal(Duration, InstigatorActor, /*bBroadcast=*/true);
}

void AGameCharacterBase::OnRep_StaggerState()
{
    // 客户端：用服务器给的时长对"本端当前时间"起倒计时。
    // 反复受击时该值会不断被刷新（每次受击都重新起算），表现为"连续挨打一直被压住"，符合预期。
    ApplyStaggerLocal(ReplicatedStaggerSeconds, /*InstigatorActor=*/nullptr, /*bBroadcast=*/true);
}

void AGameCharacterBase::ApplyStaggerLocal(float DurationSeconds, AActor* InstigatorActor, bool bBroadcast)
{
    const UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }

    // 只在"比当前剩余更久"时才延长：避免"服务器已经在倒计时、客户端 OnRep 又从头开始"这类
    // 重复起算把手感拉长（例如连续受击时两端各算各的导致时长叠加）。
    const float NewEndTime = World->GetTimeSeconds() + DurationSeconds;
    LocalStaggerEndTime = FMath::Max(LocalStaggerEndTime, NewEndTime);

    if (bBroadcast)
    {
        // 表现层用：播硬直动画/镜头抖动/音效（Lua 订阅本事件）
        OnStaggerChanged.Broadcast(DurationSeconds, InstigatorActor);
    }
}

bool AGameCharacterBase::IsStaggered() const
{
    const UWorld* World = GetWorld();
    return World && World->GetTimeSeconds() < LocalStaggerEndTime;
}

float AGameCharacterBase::GetStaggerRemainingSeconds() const
{
    const UWorld* World = GetWorld();
    return World ? FMath::Max(LocalStaggerEndTime - World->GetTimeSeconds(), 0.0f) : 0.0f;
}

// =============================================================================
//  命中 / 击杀确认（ASC → 角色事件桥接）
// =============================================================================

void AGameCharacterBase::HandleASC_HitConfirmed(AActor* HitTarget, float DamageAmount, bool bHeadshot)
{
    // 只做转发：把"服务器确认的命中"变成角色自身事件，让 Lua/HUD 无需去取 ASC 就能订阅
    OnHitConfirmed.Broadcast(HitTarget, DamageAmount, bHeadshot);
}

void AGameCharacterBase::HandleASC_KillConfirmed(AActor* VictimActor)
{
    OnKillConfirmed.Broadcast(VictimActor);
}

void AGameCharacterBase::OnDamaged_Implementation(float DamageAmount, FName HitBone, AActor* InstigatorActor)
{
    // 原生默认实现留空：受击表现全部由 Lua / 蓝图子类接管（这是"C++ 极薄、表现下沉脚本"的设计选择）。
    // 若某角色需要纯 C++ 的受击逻辑，可在其 C++ 子类中重写本函数。
}

void AGameCharacterBase::HandleHealthDeath(AActor* KillerActor)
{
    // -------------------------------------------------------------------------
    // 1) 死亡表现（各端）：先派发 OnDeath 给 Lua / 蓝图。
    //    放在"上报规则"之前的原因：死亡规则可能会立刻销毁本 Avatar，
    //    而表现（肉体松弛、碎裂特效、死亡音效）必须在角色仍然有效时同步完成。
    // -------------------------------------------------------------------------
    OnDeath(KillerActor);

    // -------------------------------------------------------------------------
    // 2) 死亡规则上报（仅服务器）：C++ 只上报，具体处置由 Lua 规则脚本决定
    //    （僵尸被击杀 → 延迟复活仍为僵尸；人类死亡 → 转观察者）。
    // -------------------------------------------------------------------------
    if (!HasAuthority())
    {
        return;
    }

    UWorld* World = GetWorld();
    AGameGameMode* GameMode = World ? World->GetAuthGameMode<AGameGameMode>() : nullptr;
    if (!GameMode)
    {
        // 非对局关卡（如测试地图）没有 AGameGameMode：不处理死亡流程，避免影响纯逻辑测试
        return;
    }

    // -------------------------------------------------------------------------
    // 3) 击杀确认（仅服务器）：把"你击杀了谁"定向发回**击杀者客户端**
    // -------------------------------------------------------------------------
    // 为什么在这里发而不是在伤害结算处：死亡是"击杀成立"的唯一权威信号，
    // 由受害者（本角色）路径发出最准确；且无需在伤害结算处查询目标血量来判断"是否致死"。
    // 说明：自杀/环境死亡（摔落）时 KillerActor 可能就是自己或为空 —— 不给自己发击杀提示。
    if (KillerActor && KillerActor != this)
    {
        const IAbilitySystemInterface* KillerAbilityInterface = Cast<IAbilitySystemInterface>(KillerActor);
        UGameAbilitySystemComponent* KillerASC = KillerAbilityInterface
            ? Cast<UGameAbilitySystemComponent>(KillerAbilityInterface->GetAbilitySystemComponent())
            : nullptr;
        if (KillerASC)
        {
            // Client RPC：由服务器发出，只到达该 ASC（击杀者）的属主客户端
            KillerASC->ClientNotifyKillConfirmed(this);
        }
    }

    GameMode->ReportPlayerDeath(GetController(), KillerActor);
}

void AGameCharacterBase::OnDeath_Implementation(AActor* KillerActor)
{
    // 原生默认实现留空：死亡表现全部由 Lua / 蓝图子类接管
    // （契约见 Content/Script/Lua/Core/Character/GameCharacterBase.lua 的 OnDeath_Implementation）。
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
    // 服务器权威：先清除本角色授予的技能，避免技能体系残留在下一个 Avatar 上
    if (HasAuthority())
    {
        if (AGamePlayerState* PS = GetPlayerState<AGamePlayerState>())
        {
            PS->ClearCharacterAbilities();
        }
    }

    // 感染变异、切换角色或死亡离身时，重置 AvatarActor，防止 GAS 回调悬空野指针
    if (UAbilitySystemComponent* ASC = GetAbilitySystemComponent())
    {
        ASC->SetAvatarActor(nullptr);
    }
    Super::UnPossessed();
}

// =============================================================================
//  体型（高矮胖瘦）与运动能力
// =============================================================================

bool AGameCharacterBase::HasMovementCapability(FGameplayTag CapabilityTag) const
{
    // 统一查询口径：无论 C++、蓝图还是 UnLua，都通过本函数判断能力，
    // 避免能力判断散落成各处 if / 枚举比较。使用 HasTag 走层级匹配（父标签命中子标签）。
    return MovementCapabilityTags.HasTag(CapabilityTag);
}

UGameWeaponComponent* AGameCharacterBase::GetWeaponComponent() const
{
    // 定义放在 .cpp：头文件中 UGameWeaponComponent 仅前向声明，此处类型完整、转换无歧义
    return WeaponComponent;
}

void AGameCharacterBase::ApplyBodyProfile()
{
    // 未配置"角色定义"或其体型档案时，保持类默认尺寸（蓝图 Components 面板中的设定），直接返回。
    const UGameCharacterBodyProfile* Profile = CharacterDefinition ? CharacterDefinition->BodyProfile : nullptr;
    if (!Profile)
    {
        return;
    }

    // 依据"形态"把尺寸灌入对应的根碰撞体。
    // 注意：形态由角色类构造期决定；若档案形态与角色实际根碰撞体不匹配，属于配置错误，需告警。
    switch (Profile->CollisionForm)
    {
    case ECharacterCollisionForm::Capsule:
        if (CollisionCapsule)
        {
            // 引擎硬约束：胶囊半高必须 >= 半径；此处主动钳制并告警，
            // 避免运行时被引擎静默夹回（这是"改了尺寸不生效"的常见疑难来源）。
            const float Radius = FMath::Max(Profile->CapsuleRadius, 1.0f);
            const float HalfHeight = FMath::Max(Profile->CapsuleHalfHeight, Radius);
            if (!FMath::IsNearlyEqual(HalfHeight, Profile->CapsuleHalfHeight))
            {
                UE_LOG(LogZombieKing, Warning,
                    TEXT("[%s] 体型档案 %s 的胶囊半高(%.1f)小于半径(%.1f)，已自动钳制为 半高=半径。"),
                    *GetName(), *Profile->GetName(), Profile->CapsuleHalfHeight, Radius);
            }
            // 第三个参数 true：立即更新重叠，保证 BeginPlay 时碰撞即按新尺寸生效
            CollisionCapsule->SetCapsuleSize(Radius, HalfHeight, true);
        }
        else
        {
            UE_LOG(LogZombieKing, Warning,
                TEXT("[%s] 体型档案 %s 声明为胶囊形态，但本类无胶囊根碰撞体，体型未生效。"),
                *GetName(), *Profile->GetName());
        }
        break;

    case ECharacterCollisionForm::Box:
        if (CollisionBox)
        {
            // 盒体尺寸下限保护：每轴至少 1cm，防止零/负尺寸导致碰撞失效
            const FVector HalfExtent = Profile->BoxHalfExtent.ComponentMax(FVector(1.0f, 1.0f, 1.0f));
            CollisionBox->SetBoxExtent(HalfExtent, true);
        }
        else
        {
            UE_LOG(LogZombieKing, Warning,
                TEXT("[%s] 体型档案 %s 声明为盒体形态，但本类未切换盒体根碰撞"
                     "（需在子类构造函数中调用 SetBoxCollisionRoot），体型未生效。"),
                *GetName(), *Profile->GetName());
        }
        break;

    default:
        // 兜底分支：枚举哨兵 MAX 等非法值，不做任何处理。
        break;
    }

    // 表现缩放：让"高矮胖瘦"同时体现在视觉上（碰撞尺寸如上，视觉体积由美术网格体缩放表达）。
    if (CharacterMesh && !Profile->MeshScale.Equals(FVector::OneVector))
    {
        CharacterMesh->SetRelativeScale3D(Profile->MeshScale);
        // TODO（后续迭代 / 交给模型同学）：若网格体原点不在脚底，体型缩放后需重新对齐脚底与眼睛位置。
    }
}

void AGameCharacterBase::ApplyMovementParams()
{
    // 未配置角色定义则保持 Mover 的类默认参数，直接返回
    if (!MoverComponent || !CharacterDefinition)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 行走速度与跳跃高度：改写 Mover 的"共享设置"对象
    // -------------------------------------------------------------------------
    // Mover 的速度/跳跃参数集中在 UCommonLegacyMovementSettings（地面与空中运动模式共用），
    // 因此改写这一处即可让所有相关模式同时生效，无需逐个模式去寻找并改写。
    if (UCommonLegacyMovementSettings* MovementSettings =
            MoverComponent->FindSharedSettings_Mutable<UCommonLegacyMovementSettings>())
    {
        // 行走最大速度（cm/s）
        MovementSettings->MaxSpeed = FMath::Max(CharacterDefinition->WalkSpeed, 0.0f);

        // 跳跃：策划按"跳多高(cm)"配置，而 Mover 需要的是"起跳初速(cm/s)"，按 v = sqrt(2 * g * h) 换算。
        // 重力取世界重力（Mover 的起跳/落地同样受世界重力影响），保证换算与实际手感一致。
        if (CharacterDefinition->JumpHeight > 0.0f)
        {
            const float GravityZ = GetWorld() ? FMath::Abs(GetWorld()->GetGravityZ()) : 0.0f;
            if (GravityZ > 0.0f)
            {
                MovementSettings->JumpUpwardsSpeed = FMath::Sqrt(2.0f * GravityZ * CharacterDefinition->JumpHeight);
            }
        }
    }
    else
    {
        // 找不到共享设置属于装配异常（例如 Mover 组件未挂载默认运动模式集），显式告警便于定位
        UE_LOG(LogZombieKing, Warning,
            TEXT("[%s] 未找到 Mover 共享设置（UCommonLegacyMovementSettings）：行走速度/跳跃高度未生效，"
                 "请确认 Mover 组件已挂载默认运动模式集。"),
            *GetName());
    }
}

void AGameCharacterBase::SetBoxCollisionRoot(const FVector& InHalfExtent)
{
    // 本函数必须由子类"构造函数"调用：
    //  1) 组件创建只能在构造期完成；
    //  2) 根组件必须在此刻定好——否则 Mover 初始化时 FindDefaultUpdatedComponent() 会锁定错误的根组件
    //     （引擎实现：UpdatedComponent 取自 Owner->GetRootComponent()），且运行期再换根会导致预测管线错乱。
    CollisionBox = CreateDefaultSubobject<UBoxComponent>(TEXT("CollisionBox"));
    CollisionBox->SetBoxExtent(InHalfExtent);
    // 与胶囊保持一致的碰撞配置：Pawn 预设（Mover 位移扫掠依赖 Pawn 通道响应）
    CollisionBox->SetCollisionProfileName(UCollisionProfile::Pawn_ProfileName);
    CollisionBox->CanCharacterStepUpOn = ECB_No;
    CollisionBox->SetShouldUpdatePhysicsVolume(true);

    // 切换根组件：引擎会自动把原根（胶囊）挂到新根（盒体）之下，无需手动 SetupAttachment。
    SetRootComponent(CollisionBox);

    // 胶囊降级为盒体的子组件并停用碰撞：
    // 保留其存在，是为了不破坏基类及子类对 CollisionCapsule 的既有引用（如 AZombieCrawlerCharacter 的尺寸设置）；
    // 但必须停用碰撞，否则胶囊与盒体会同时参与判定，产生重复命中与双重阻挡。
    if (CollisionCapsule)
    {
        CollisionCapsule->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }
}
