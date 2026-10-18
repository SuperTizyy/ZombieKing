#include "ZombieCrawlerCharacter.h"
#include "CrawlerMoverComponent.h"
#include "CrawlerClimbMode.h"
#include "Components/CapsuleComponent.h"
#include "GameCharacterTags.h"     // 运动能力原生标签（多足 / 爬墙）
#include "GameCharacterDefinition.h" // 角色定义（爬墙速度等移动参数来源，需完整类型）

AZombieCrawlerCharacter::AZombieCrawlerCharacter(const FObjectInitializer& ObjectInitializer)
    // 关键：把基类挂的通用 MoverComponent 替换为多足专属组件。
    // 子类组件在构造阶段完成"爬墙模式 + 全局吸附过渡"的组装（官方推荐的模式注册方式），
    // 组件名必须与基类 CreateDefaultSubobject 使用的名字（"MoverComponent"）一致。
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UCrawlerMoverComponent>(TEXT("MoverComponent")))
{
    // 1. 指定绑定的 UnLua 脚本路径（相对 Content/Script，文件位于 Lua/Core/Character/zombie/）
    //    注意：目录名大小写必须与磁盘一致（Android/iOS 文件系统区分大小写，Windows 不区分但勿依赖）
    LuaScriptPath = TEXT("Lua.Core.Character.Zombie.ZombieCrawlerCharacter");

    // 2. 多足底盘默认形状（仅为类默认值，蓝图子类可在 Components 面板中自由覆盖）：
    //    引擎硬约束：胶囊体 CapsuleHalfHeight >= CapsuleRadius，
    //    在蓝图里把半径改得比半高大时会被引擎自动夹回（这是改不动尺寸的常见原因）；
    //    若确需"真正的宽扁底盘"，应改用 UBoxComponent 作为根组件（Mover 支持任意图元）。
    if (CollisionCapsule)
    {
        CollisionCapsule->SetCapsuleSize(55.0f, 60.0f);
    }

    // 3. 配置真第一人称相机头部挂点：多足生物眼睛处于头胸部前侧
    HeadBoneName = FName("Spider_Head");

    // 4. 自由视角：Mover 由同步状态驱动角色姿态（爬墙姿态 = Up 对齐墙面法线），
    //    禁止 Controller 旋转直接改写 Actor 朝向，避免与运动模式姿态解算打架
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;

    // 5. 运动能力声明（严格对齐游戏定义）：多足形态 + 具备爬墙能力。
    //    说明：标签仅作"能力声明"，真正的爬墙实现由 UCrawlerMoverComponent 的 Climbing 模式提供；
    //          二者必须保持一致，否则会出现"标签说能爬、实际爬不了"的错配。
    //    注意：刻意在 C++ 构造函数中声明（而非蓝图默认值），避免被蓝图子类"另存默认值"覆盖；
    //          查询统一走基类 HasMovementCapability()。
    MovementCapabilityTags.AddTag(GameCharacterTags::Capability_Multiped);
    MovementCapabilityTags.AddTag(GameCharacterTags::Capability_Climb);

    // 说明：移动/视角/跳跃/攻击的输入资产（IMC 与 IA）在基类 AGameCharacterBase 中声明，
    //      请在蓝图子类 BP_ZombieCrawlerCharacter 的 Class Defaults → Input 分类下指定。
}

void AZombieCrawlerCharacter::ApplyMovementParams()
{
    // 先应用基类的行走速度与跳跃高度（改写 Mover 共享设置）
    Super::ApplyMovementParams();

    if (!MoverComponent || !CharacterDefinition)
    {
        return;
    }

    // 爬墙速度属于本模式的私有参数，不经过 Mover 共享设置，因此需要按类型定位到模式实例后写入。
    // FindMovementMode 按模式类查找已注册的模式实例（模式在 UCrawlerMoverComponent 构造期已注册）。
    if (UCrawlerClimbMode* ClimbMode = Cast<UCrawlerClimbMode>(MoverComponent->FindMovementMode(UCrawlerClimbMode::StaticClass())))
    {
        ClimbMode->MaxClimbSpeed = FMath::Max(CharacterDefinition->ClimbSpeed, 0.0f);
    }
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
