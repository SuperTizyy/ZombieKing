#include "HealthComponent.h"
#include "AbilitySystemComponent.h"
#include "GameAttributeSet.h"              // 属性集：GetHealthAttribute / GetMaxHealthAttribute
#include "GameplayEffect.h"                // FGameplayEffectSpec（读取击杀者归因）
#include "GameplayEffectExtension.h"       // FGameplayEffectModCallbackData 完整定义

UHealthComponent::UHealthComponent()
{
    // 纯逻辑组件：所有回调由 GAS 属性委托事件驱动，严禁启用 Tick
    PrimaryComponentTick.bCanEverTick = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
}

void UHealthComponent::InitializeWithAbilitySystem(UAbilitySystemComponent* InASC)
{
    if (!InASC)
    {
        return;
    }

    // 先解绑旧订阅：PossessedBy（服务器）与 OnRep_PlayerState（客户端）都可能触发本函数，
    // 不去重会导致同一次属性变化被多次转发（事件重复、表现抖动）。
    UnbindAbilitySystemDelegates();

    // 缓存 ASC（弱引用）：后续只读查询与解绑都依赖它
    AbilitySystemComponent = InASC;

    // 订阅生命值与生命上限的变化：
    //  - GAS 的属性变化委托是非动态多播委托，因此用 AddUObject 绑定普通 C++ 方法；
    //  - 句柄必须保存，用于 EndPlay 解绑（否则组件销毁后回调悬空 → 野指针崩溃）。
    HealthChangedDelegateHandle = InASC
        ->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetHealthAttribute())
        .AddUObject(this, &UHealthComponent::HandleHealthChanged);

    MaxHealthChangedDelegateHandle = InASC
        ->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetMaxHealthAttribute())
        .AddUObject(this, &UHealthComponent::HandleMaxHealthChanged);
}

float UHealthComponent::GetHealth() const
{
    // 只读查询当前"最终值"（含 GameplayEffect 修饰符），与 UI 显示口径一致
    UAbilitySystemComponent* ASC = AbilitySystemComponent.Get();
    return ASC ? ASC->GetNumericAttribute(UGameAttributeSet::GetHealthAttribute()) : 0.0f;
}

float UHealthComponent::GetMaxHealth() const
{
    UAbilitySystemComponent* ASC = AbilitySystemComponent.Get();
    return ASC ? ASC->GetNumericAttribute(UGameAttributeSet::GetMaxHealthAttribute()) : 0.0f;
}

float UHealthComponent::GetHealthPercent() const
{
    const float MaxHealth = GetMaxHealth();
    // 上限为 0（属性集尚未初始化）时返回 0，避免除零
    return (MaxHealth > 0.0f) ? FMath::Clamp(GetHealth() / MaxHealth, 0.0f, 1.0f) : 0.0f;
}

bool UHealthComponent::IsDead() const
{
    return GetHealth() <= 0.0f;
}

void UHealthComponent::HandleHealthChanged(const FOnAttributeChangeData& Data)
{
    // Delta = 新值 - 旧值：正数为治疗、负数为受伤，供飘字与受击反馈判断方向
    const float Delta = Data.NewValue - Data.OldValue;
    const float MaxHealth = GetMaxHealth();

    // 转发为项目事件（表现层由 Lua 订阅，C++ 侧不做任何表现假设）
    OnHealthChanged.Broadcast(Data.NewValue, MaxHealth, Delta);

    if (Data.NewValue <= 0.0f)
    {
        // 死亡去重：生命归零只广播一次（预测回滚/多次扣血都可能重复触发本回调）
        if (!bDeathBroadcast)
        {
            bDeathBroadcast = true;

            // 击杀者归因：从触发本次变化的 GameplayEffect 上下文中取 Instigator
            AActor* KillerActor = nullptr;
            if (Data.GEModData)
            {
                KillerActor = Data.GEModData->EffectSpec.GetContext().GetInstigator();
            }
            OnDeath.Broadcast(KillerActor);
        }
    }
    else
    {
        // 复活/回血到正值：重置去重标志，供下一回合或复活流程复用
        bDeathBroadcast = false;
    }
}

void UHealthComponent::HandleMaxHealthChanged(const FOnAttributeChangeData& Data)
{
    // 生命上限变化：Delta 传 0（本次并非"扣血/回血"），仅用于驱动 UI 按新上限重算血条百分比
    OnHealthChanged.Broadcast(GetHealth(), Data.NewValue, 0.0f);
}

void UHealthComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 组件销毁前解绑委托：这是防止"角色销毁后 ASC 回调悬空崩溃"的关键一步
    UnbindAbilitySystemDelegates();

    Super::EndPlay(EndPlayReason);
}

void UHealthComponent::UnbindAbilitySystemDelegates()
{
    if (UAbilitySystemComponent* ASC = AbilitySystemComponent.Get())
    {
        if (HealthChangedDelegateHandle.IsValid())
        {
            ASC->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetHealthAttribute())
               .Remove(HealthChangedDelegateHandle);
        }
        if (MaxHealthChangedDelegateHandle.IsValid())
        {
            ASC->GetGameplayAttributeValueChangeDelegate(UGameAttributeSet::GetMaxHealthAttribute())
               .Remove(MaxHealthChangedDelegateHandle);
        }
    }

    HealthChangedDelegateHandle.Reset();
    MaxHealthChangedDelegateHandle.Reset();
}
