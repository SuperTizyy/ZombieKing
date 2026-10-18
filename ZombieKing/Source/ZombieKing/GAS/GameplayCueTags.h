#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"   // FNativeGameplayTag 与 UE_DECLARE/DEFINE_GAMEPLAY_TAG 宏

/**
 * 竞技生化网游：GameplayCue（**表现通道**）原生标签集
 *
 * 为什么需要这一组标签：
 *  - GameplayCue 是 GAS 官方的"表现"通道：命中的粒子/音效/弹道/镜头震动等**表现**，
 *    由 GameplayCue 统一分发，而不是各业务代码自己 new 特效；
 *  - 本文件登记本项目用到的 Cue 标签，使"内容侧资产名 → 标签"的编辑器自动推导可用
 *    （推导规则见下方"内容侧命名约定"），也让 C++ / Lua / GE 资产用同一套标签引用表现。
 *
 * 与其它标签文件的分工：
 *  - GameCharacterTags —— 角色"运动能力"标签（双足 / 多足 / 爬墙）；
 *  - GameEffectTags    —— "数据/效果"标签（SetByCaller 传参，如 Data.Damage）；
 *  - GameAbilityTags   —— "技能输入"标签（Ability.Input.Attack）；
 *  - 本文件            —— "表现"标签（GameplayCue.*）。
 *
 * 内容侧命名约定（已核实引擎实现 UAbilitySystemGlobals::DeriveGameplayCueTagFromAssetName）：
 *  - 编辑器会把 Cue 通知资产名做变换来**自动推导**标签：去掉 `GC_` 前缀与 `_c` 后缀、
 *    把 `_` 换成 `.`、若不含 "GameplayCue" 则前补 `GameplayCue.`；
 *  - 因此资产命名规则是：`GC_<标签名去掉 GameplayCue. 前缀、点换下划线>`，例如
 *      GC_Damage_Head  → GameplayCue.Damage.Head
 *      GC_Zombie_Infected → GameplayCue.Zombie.Infected
 *  - 资产需放在 GameplayCueNotifyPaths 扫描目录下（引擎默认 `/Game/GameplayCues`）；
 *  - 派生对象建议用本项目的 Cue 基类（UGameGameplayCueNotify_Static / AGameGameplayCueNotify_Actor），
 *    以便表现逻辑热更到 Lua。
 */
namespace GameplayCueTags
{
    // -------------------------------------------------------------------------
    // 命中表现（由伤害 GE 的 GameplayCues 数组触发；部位差异由 Cue 脚本按上下文决定）
    // -------------------------------------------------------------------------
    /** 通用命中：部位不明或无需区分时使用 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Impact);
    /** 头部命中：暴击感表现（特殊音效/血雾/更重的镜头反馈） */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Head);
    /** 躯干命中 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Torso);
    /** 四肢命中 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Damage_Limb);

    // -------------------------------------------------------------------------
    // 玩法关键时刻表现
    // -------------------------------------------------------------------------
    /** 被感染瞬间（人类 → 生化）：变异演出 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Zombie_Infected);
    /** 角色死亡 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Character_Death);
    /** 角色复活 / 重新生成 */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Character_Respawn);
}
