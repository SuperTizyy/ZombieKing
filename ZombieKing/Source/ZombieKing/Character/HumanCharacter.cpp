#include "HumanCharacter.h"
#include "Components/CapsuleComponent.h"
#include "GameCharacterTags.h"     // 运动能力原生标签（双足）

AHumanCharacter::AHumanCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 1. 运动能力声明（严格对齐游戏定义）：人类仅双足，且不具备爬墙能力。
    //    刻意只加 Biped，不加 Climb —— 与"人类无法爬墙"的设定保持一致。
    MovementCapabilityTags.AddTag(GameCharacterTags::Capability_Biped);

    // 2. 人类体型默认值（胶囊体）：仅作类默认尺寸，
    //    真正的"高矮胖瘦"由 AGameCharacterBase::BodyProfile 资产在 BeginPlay 统一灌入并覆盖。
    //    引擎硬约束：胶囊半高 >= 半径。
    if (CollisionCapsule)
    {
        CollisionCapsule->SetCapsuleSize(40.0f, 90.0f);
    }

    // 3. 真第一人称头部骨骼挂点名（人类标准骨架）
    HeadBoneName = FName("head");

    // 说明：不在此覆写 LuaScriptPath，直接继承基类的通用脚本路径
    //      （人类与母体在玩法表现上差异较大时，再各自指定专属 Lua 脚本）。
}
