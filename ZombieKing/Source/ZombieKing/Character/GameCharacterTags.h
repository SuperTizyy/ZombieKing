#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"   // FNativeGameplayTag 与 UE_DECLARE/DEFINE_GAMEPLAY_TAG 宏

/**
 * 竞技生化网游：角色"运动能力"原生标签集（Native GameplayTags）
 *
 * 为什么用 GameplayTags 而不是布尔/枚举：
 *  - 能力可自由组合（"多足 + 爬墙"是两个独立标签，而非枚举里硬编码的一个组合值）；
 *  - 全项目（C++ / 蓝图 / UnLua / GAS / AI / UI）共用同一套标签寻址，查询口径统一；
 *  - 与项目已启用的 GameplayTags 依赖天然契合，后续可直接作为 GAS 的条件标签使用。
 *
 * 命名规范：标签字符串为 Character.Capability.<能力名>；
 *           C++ 变量名额外加 Capability_ 前缀（仅变量名，标签字符串本身不含该前缀）。
 *
 * 使用约定（严格对齐游戏定义）：
 *  - 人类幸存者：仅 Capability_Biped；
 *  - 生化幽灵：Capability_Biped 或 Capability_Multiped 二者之一（取决于角色形态）；
 *  - 可爬墙的多足生化幽灵：Capability_Multiped + Capability_Climb（"部分多足"才带 Climb）。
 */
namespace GameCharacterTags
{
    /** 双足运动能力：人类与双足生化幽灵共有 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Capability_Biped);

    /** 多足运动能力：仅多足生化幽灵拥有 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Capability_Multiped);

    /** 爬墙能力：仅"部分多足生化幽灵"拥有（须配合 UCrawlerMoverComponent 的爬墙模式） */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Capability_Climb);
}
