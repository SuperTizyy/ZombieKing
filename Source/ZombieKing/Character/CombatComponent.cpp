#include "CombatComponent.h"

UCombatComponent::UCombatComponent()
{
    // 纯逻辑组件：由受击事件/委托驱动，不启用 Tick
    PrimaryComponentTick.bCanEverTick = false;
}
