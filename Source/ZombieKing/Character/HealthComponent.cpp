#include "HealthComponent.h"
#include "AbilitySystemComponent.h"

UHealthComponent::UHealthComponent()
{
    // 纯逻辑组件：所有回调由 GAS 属性委托事件驱动，严禁启用 Tick
    PrimaryComponentTick.bCanEverTick = false;
}

void UHealthComponent::InitializeWithAbilitySystem(UAbilitySystemComponent* InASC)
{
    AbilitySystemComponent = InASC;

    // TODO: 后续迭代在此订阅属性变化委托，例如：
    //  - AbilitySystemComponent->GetGameplayAttributeValueChangeDelegate(HealthAttribute)
    //       .AddUObject(this, &UHealthComponent::HandleHealthChanged);
    //  - 监听死亡标签/属性归零，广播 OnDeath 至 Lua 死亡演出流程
}
