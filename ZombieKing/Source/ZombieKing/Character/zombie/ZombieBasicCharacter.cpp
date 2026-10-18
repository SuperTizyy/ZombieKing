#include "ZombieBasicCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GameCharacterTags.h"     // 运动能力原生标签（双足）

AZombieBasicCharacter::AZombieBasicCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 1. 运动能力声明（严格对齐游戏定义）：双足形态、不可爬墙。
    //    与 AZombieCrawlerCharacter 同理，刻意在 C++ 构造期声明（而非蓝图默认值），
    //    避免"标签说能爬/不能爬"与实际运动组件错配；查询统一走基类 HasMovementCapability()。
    MovementCapabilityTags.AddTag(GameCharacterTags::Capability_Biped);

    // 2. 体型默认值（胶囊体）：仅作类默认尺寸（中型，与人类 1:1 量级），
    //    真正的体型由角色定义资产引用的 BodyProfile 在 BeginPlay 灌入（改资产即生效）。
    if (CollisionCapsule)
    {
        CollisionCapsule->SetCapsuleSize(40.0f, 90.0f);
    }

    // 3. 自由视角：与爬墙怪同理，姿态由 Mover 同步状态驱动，禁止 Controller 旋转直接改写 Actor 朝向
    bUseControllerRotationPitch = false;
    bUseControllerRotationYaw = false;
    bUseControllerRotationRoll = false;

    // 说明：
    //  - 本类暂不绑定专属 Lua 脚本（LuaScriptPath 留空，与 AZombieMotherCharacter 同策略），
    //    表现继承基类 GameCharacterBase.lua；将来基础幽灵有专属表现（音效/动画差异）时再配专属脚本；
    //  - 输入资产（IMC 与 IA）在蓝图子类 BP_ZombieBasicCharacter 的 Class Defaults → Input 下指定；
    //  - 血量/移速/跳跃/技能集全部来自角色定义资产（UGameCharacterDefinition），由内容侧配置。
}
