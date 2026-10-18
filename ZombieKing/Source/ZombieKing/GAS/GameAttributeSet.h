#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"               // GAS 属性集基类，以及 DOREPLIFETIME_CONDITION_NOTIFY / GAMEPLAYATTRIBUTE_REPNOTIFY 等引擎宏
#include "AbilitySystemComponent.h"     // FGameplayAttribute、FGameplayEffectModCallbackData 等 GAS 核心类型
#include "GameAttributeSet.generated.h" // UHT 生成的头文件，必须位于所有 include 的最后一行

/**
 * 属性访问器组合宏（项目内部约定，统一以 GZ_ 前缀命名以避免与引擎/三方同名宏冲突）
 *
 * 目的：为一个 FGameplayAttributeData 属性一次性生成四组标准函数，避免逐属性手写造成遗漏与不一致：
 *   1) GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName)
 *      —— 生成静态函数 Get<PropertyName>Attribute()，返回 FGameplayAttribute，
 *         供 GameplayEffect / 能力通过反射精确指向该属性（GAS 数值改动的唯一入口）；
 *   2) GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName)
 *      —— 生成 Get<PropertyName>()，以 float 返回当前基础值，供技能/UI 只读查询；
 *   3) GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName)
 *      —— 生成 Set<PropertyName>(float)，设置基础值；内部经 ASC 走 PreAttributeChange 钳制，
 *         仅允许在服务器权威侧调用，客户端调用会被 GAS 忽略；
 *   4) GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)
 *      —— 生成 Init<PropertyName>(float)，初始化基础值，用于角色生成时灌入初始数值。
 *
 * 说明：底层 GAMEPLAYATTRIBUTE_* 宏由引擎 GameplayAbilities 模块的 AttributeSet.h 提供，
 *       本宏只做组合封装，不重复实现引擎能力。
 */
#define GZ_ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/**
 * 竞技生化网游：属性集公共基类（GAS AttributeSet / 通用属性契约）
 *
 * 架构定位（工业级 GAS 规范）：
 *  - AttributeSet 是"所有数值的唯一权威来源"：生命等数值一律经 GAS 流动，
 *    严禁在 Pawn / PlayerState / 组件上散落裸属性——否则复制、预测、回滚会全面失控；
 *  - 所有数值变更必须由 GameplayEffect 在服务器结算，客户端只做只读监听与表现；
 *  - 本类只承载"人类与生化幽灵共有的通用属性"，各自专有属性由子类承载：
 *        UHumanAttributeSet —— 人类幸存者专有属性；
 *        UZombieAttributeSet —— 生化幽灵专有属性。
 *
 * 热更新边界（竞技网游铁律）：
 *  - 本类（C++）只定义"不可热更的数据结构与复制协议"——属性字段、复制声明、钳制规则；
 *  - 具体数值（初始生命、成长曲线）一律由 GameplayEffect / 数据资产在 Lua 侧配置，
 *    保证线上可不停机调参，无需重新编译 C++。
 *
 * 复制策略说明：
 *  - 通用属性对全网复制（COND_None）：远端需要看到他人生命以驱动命中反馈与头顶血条；
 *    而技能、冷却、GameplayEffect 内部状态则由 ASC 的 Mixed 复制模式限制为仅拥有者可见。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UGameAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	/**
	 * 构造函数。
	 * 属性集构造阶段不设置任何数值：初始数值必须由"初始化 GameplayEffect"在服务器生成角色时统一灌入，
	 * 以保证数值来源单一、可预测、可回放（直接在此写死数值会导致客户端与服务端不一致）。
	 */
	UGameAttributeSet();

	// -------------------------------------------------------------------------
	//  网络复制
	// -------------------------------------------------------------------------
	/**
	 * 注册需要网络复制的属性。
	 * 引擎在属性集注册到 ASC 后调用；必须显式声明每个复制属性，
	 * 否则属性只在服务器可见——客户端将永远拿不到数值（UI 恒显示为 0）。
	 */
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// -------------------------------------------------------------------------
	//  GAS 数值变更钩子（服务器权威钳制点）
	// -------------------------------------------------------------------------
	/**
	 * 属性"基础值"被修改前的回调（PreAttributeChange），主要用于钳制取值范围。
	 * 注意：此处只改基础值，不处理 GameplayEffect 施加的瞬时/持续修饰符最终值；
	 *       最终值的钳制应放在 PostGameplayEffectExecute 中进行。
	 */
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	/**
	 * GameplayEffect 执行完毕后的回调（PostGameplayEffectExecute），用于在权威结算后钳制最终值。
	 * 这是"服务器权威"的最后一道闸门：任何越界数值（负血量、超上限等）都在此被纠正后再复制出去，
	 * 从而保证所有客户端看到的数值一定是合法值。
	 */
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	// =========================================================================
	//  通用属性 —— 生命（Health）
	// =========================================================================

	/** 当前生命值：<=0 判定死亡；仅由 GameplayEffect 修改，绝不由业务代码直接写 */
	UPROPERTY(BlueprintReadOnly, Category = "Attribute|Vitals", ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;
	/** 生成 GetHealthAttribute() / GetHealth() / SetHealth() / InitHealth() 四件套 */
	GZ_ATTRIBUTE_ACCESSORS(UGameAttributeSet, Health)

	/** 生命上限：生命值的钳制上界，随角色类型/装备变化（改它会引发血条比例重算） */
	UPROPERTY(BlueprintReadOnly, Category = "Attribute|Vitals", ReplicatedUsing = OnRep_MaxHealth)
	FGameplayAttributeData MaxHealth;
	GZ_ATTRIBUTE_ACCESSORS(UGameAttributeSet, MaxHealth)

protected:
	// -------------------------------------------------------------------------
	//  复制回调（OnRep）：仅在客户端触发，用于把服务器数值变化同步给表现层
	//  统一声明为 virtual，便于子类在不破坏基类契约的前提下追加表现逻辑
	// -------------------------------------------------------------------------

	/** 生命值复制回调：通知 GAS 派发属性变更事件，驱动血条/受击表现刷新 */
	UFUNCTION()
	virtual void OnRep_Health(const FGameplayAttributeData& OldValue);

	/** 生命上限复制回调：保留钩子，供"上限变化后重算血条比例"等表现逻辑接入 */
	UFUNCTION()
	virtual void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
};
