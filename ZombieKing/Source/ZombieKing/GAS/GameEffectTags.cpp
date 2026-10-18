#include "GameEffectTags.h"

// 原生标签的"定义"必须放在 .cpp：
//  - UE_DEFINE_GAMEPLAY_TAG 内含 static_assert 校验文件扩展名，只允许在 .cpp 中使用；
//  - 每个定义与头文件中的 UE_DECLARE_GAMEPLAY_TAG_EXTERN 声明一一对应；
//  - 标签在本模块静态构造阶段自动注册进 GameplayTags 管理器，随模块卸载自动注销。
namespace GameEffectTags
{
    // SetByCaller：原始伤害值
    UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage");

    // 伤害来源标记：近战致命一击（刀杀判定链路，见头文件注释）
    UE_DEFINE_GAMEPLAY_TAG(Damage_Source_Melee, "Damage.Source.Melee");

    // SetByCaller：进化累计生命上限增量
    UE_DEFINE_GAMEPLAY_TAG(Data_ZombieMaxHealthDelta, "Data.Zombie.MaxHealthDelta");
}
