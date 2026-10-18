#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"   // FNativeGameplayTag 与 UE_DECLARE/DEFINE_GAMEPLAY_TAG 宏

/**
 * 竞技生化网游：技能/输入相关原生标签集（Native GameplayTags）
 *
 * 与其它标签文件的分工：
 *  - GameCharacterTags —— 角色"运动能力"标签（双足/多足/爬墙）；
 *  - GameEffectTags    —— "数据/效果"类标签（SetByCaller 伤害值等）；
 *  - 本文件            —— "技能输入"类标签，用于把按键与技能解耦。
 *
 * 命名规范：标签字符串为 Ability.Input.<用途>（与 UGameInputConfig 中 InputTag 的 meta 分类一致）；
 *           C++ 变量名额外加 Input_ 前缀（仅变量名，标签字符串本身不含该前缀）。
 *
 * 使用约定（三步走，见 UGameInputConfig 的说明）：
 *  1) 在输入配置资产（UGameInputConfig）里把某个 InputAction 映射到本文件的输入标签；
 *  2) 在具体技能的蓝图子类 AbilityTags 中加入同一标签；
 *  3) 角色按下该键 → AGameCharacterBase::BindAbilityInputActions 按标签激活技能（引擎内建机制）。
 * 之所以"标签"而不是"按键枚举"：同一个按键在不同角色上可以激活不同技能（每只生化幽灵技能不同）。
 */
namespace GameAbilityTags
{
    /**
     * 主攻击输入标签（字符串：Ability.Input.Attack）。
     * 约定：人类的"射击"与生化幽灵的"近战爪击"都用此标签，
     *       由各角色的输入配置资产映射到各自的手感按键上。
     */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Input_Attack);

    /**
     * 开火输入标签（字符串：Ability.Input.Fire）—— 武器系统专用。
     *
     * 与 Input_Attack 的分工（两者都要保留，不可合并）：
     *  - `Ability.Input.Attack` = "通用攻击"：近战爪击等**不依赖武器**的攻击用它；
     *  - `Ability.Input.Fire`   = "武器开火"：**依赖武器组件当前武器定义**的射击用它，
     *    因此只有配了武器槽的角色（通常是人类）才需要映射这个键。
     * 分开的理由：同一角色可以同时拥有近战与开火（例如生化幽灵被感染前的副武器），
     *            且"开火"需要沿用到武器定义的射速/弹药，而"通用攻击"不需要武器。
     */
    UE_DECLARE_GAMEPLAY_TAG_EXTERN(Input_Fire);
}
