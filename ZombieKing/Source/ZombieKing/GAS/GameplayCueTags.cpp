#include "GameplayCueTags.h"

// 原生标签的"定义"必须放在 .cpp：
//  - UE_DEFINE_GAMEPLAY_TAG 内含 static_assert 校验文件扩展名，只允许在 .cpp 中使用；
//  - 每个定义与头文件中的 UE_DECLARE_GAMEPLAY_TAG_EXTERN 声明一一对应；
//  - 标签在本模块静态构造阶段自动注册进 GameplayTags 管理器，随模块卸载自动注销。
namespace GameplayCueTags
{
    // ----- 命中表现 -----
    UE_DEFINE_GAMEPLAY_TAG(Damage_Impact, "GameplayCue.Damage.Impact");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Head, "GameplayCue.Damage.Head");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Torso, "GameplayCue.Damage.Torso");
    UE_DEFINE_GAMEPLAY_TAG(Damage_Limb, "GameplayCue.Damage.Limb");

    // ----- 玩法关键时刻表现 -----
    UE_DEFINE_GAMEPLAY_TAG(Zombie_Infected, "GameplayCue.Zombie.Infected");
    UE_DEFINE_GAMEPLAY_TAG(Character_Death, "GameplayCue.Character.Death");
    UE_DEFINE_GAMEPLAY_TAG(Character_Respawn, "GameplayCue.Character.Respawn");
}
