#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"   // FNativeGameplayTag 与 UE_DECLARE/DEFINE_GAMEPLAY_TAG 宏

/**
 * 竞技生化网游：GameplayEffect / SetByCaller 数据标签集（原生标签）
 *
 * 与 GameCharacterTags 的分工：
 *  - GameCharacterTags —— 角色"运动能力"标签（双足/多足/爬墙）；
 *  - 本文件 —— "数据/效果"类标签，当前用于 SetByCaller 传参（伤害值等）。
 *
 * SetByCaller 机制说明：
 *  伤害值等"每次结算都不同"的数值，不适合写死在 GameplayEffect 资产里，
 *  而是由施加方在效果规格上通过 SetByCaller 写入（见 UGameAbilitySystemComponent::ApplyDamageToTarget），
 *  再由执行计算（UGameDamageExecution）读取。这样同一份伤害 GE 资产可服务所有伤害来源。
 */
namespace GameEffectTags
{
    /** SetByCaller：本次结算的"原始伤害值"（由伤害来源在效果规格上写入，执行计算读取） */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);

    /**
     * 伤害来源标记："近战（刀/爪）致命一击"。
     * 由近战链路在施加伤害时写入效果规格的**动态资产标签**（AddDynamicAssetTag），
     * 受击方（UCombatComponent）在属性变化回调里读 GetAllAssetTags 判定"本次是否近战命中"，
     * 角色据此缓存"最近一次受击是否近战"，供 Lua 死亡规则实现 CF 式"刀杀禁令"
     * （被刀杀的生化幽灵本回合禁复活）。
     * 内容侧零配置：标签随近战技能代码路径自动携带，不需要给 GE 资产填 Asset Tags。
     */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Source_Melee);

    /**
     * SetByCaller：生化幽灵"进化累计生命上限增量"（由角色进化落地接口写入共享 GE 规格）。
     * 进化 GE 资产（内容侧一份共享）以加法修饰符消费本值：MaxHealth += SetByCaller，Health += SetByCaller
     * —— 升级既抬上限也立即回等额血（CF 式进化增益）。数值表在角色定义资产（逐幽灵不同）。
     */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_ZombieMaxHealthDelta);
}
