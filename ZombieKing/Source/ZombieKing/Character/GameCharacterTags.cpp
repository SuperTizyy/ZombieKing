#include "GameCharacterTags.h"

// 原生标签的"定义"必须放在 .cpp：
//  - UE_DEFINE_GAMEPLAY_TAG 内部含 static_assert 校验文件扩展名，只允许在 .cpp 中使用；
//  - 每个定义与头文件中的 UE_DECLARE_GAMEPLAY_TAG_EXTERN 声明一一对应。
// 标签会在本模块静态构造阶段自动注册进 GameplayTags 管理器，随模块卸载自动注销。
namespace GameCharacterTags
{
    // 双足运动能力
    UE_DEFINE_GAMEPLAY_TAG(Capability_Biped, "Character.Capability.Biped");

    // 多足运动能力
    UE_DEFINE_GAMEPLAY_TAG(Capability_Multiped, "Character.Capability.Multiped");

    // 爬墙能力
    UE_DEFINE_GAMEPLAY_TAG(Capability_Climb, "Character.Capability.Climb");
}
