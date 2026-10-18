#include "ZombieMotherCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GameCharacterTags.h"     // 运动能力原生标签（双足）

AZombieMotherCharacter::AZombieMotherCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // -------------------------------------------------------------------------
    // 玩法定位（用户拍板，2026 母体形态定稿）：**压制型母体（CF 经典）**
    //   大体积 + 高血量 + 移速略快于人类 + 重击方向技能，双足、不可爬墙。
    //   定位"推进堡垒"：人类打得动但必须集火，母体靠血量与体型硬推进点。
    //   具体数值不在 C++：血量走角色定义资产的初始化 GameplayEffect，
    //   移速/跳跃走角色定义资产的 WalkSpeed/JumpHeight，体型走 BodyProfile（改资产即生效）。
    // -------------------------------------------------------------------------

    // 运动能力声明：双足形态、不可爬墙（标签与实际能力保持一致；不换装爬墙运动组件）。
    // 刻意在 C++ 构造期声明（而非蓝图默认值），查询统一走基类 HasMovementCapability()。
    MovementCapabilityTags.AddTag(GameCharacterTags::Capability_Biped);

    // 母体体型默认值（胶囊体）：仅作类默认尺寸（压制型 = 大体积，比人类约大 1.4 倍量级），
    // 真正的体型由 BodyProfile 资产在 BeginPlay 灌入。
    if (CollisionCapsule)
    {
        CollisionCapsule->SetCapsuleSize(55.0f, 120.0f);
    }
}
