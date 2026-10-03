#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HealthComponent.generated.h"

class UAbilitySystemComponent;

/**
 * 竞技生化网游：生命状态组件（占位空壳版）
 *
 * 架构定位：
 *  - 只读监听 GAS 生命属性（Health/Armor）的变化，向 HUD/伤害数字/受击表现广播；
 *  - 不承载任何权威伤害计算（伤害数值一律由 GAS GameplayEffect 在服务器结算）。
 *
 * TODO（后续迭代补全）：
 *  - 绑定 ASC 的 AttributeChange 委托（Health/Armor），派发 OnHealthChanged/OnDeath 事件给 Lua；
 *  - 死亡生命周期：通知 GameMode 进入击杀结算 / 生化幽灵的复活或销毁策略；
 *  - 变异（人类→幽灵）时生命属性集的整体切换处理。
 */
UCLASS()
class ZOMBIEKING_API UHealthComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UHealthComponent();

    /**
     * 与 GAS 能力系统建立订阅关系（在 PlayerState ASC 就绪后由角色基类调用）
     * 当前为空实现（占位），仅缓存 ASC 以便后续迭代。
     */
    void InitializeWithAbilitySystem(UAbilitySystemComponent* InASC);

protected:
    /** 缓存的 GAS 组件（弱引用，避免生命周期悬空） */
    TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;
};
