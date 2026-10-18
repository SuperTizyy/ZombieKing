#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"   // FOnAttributeChangeData（GAS 属性变化数据结构）
#include "HealthComponent.generated.h"

class UAbilitySystemComponent;
struct FOnAttributeChangeData;

/** 生命变化广播：NewHealth/ MaxHealth 为最新数值，Delta 为本次变化量（供飘字与受击反馈） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnHealthChangedSignature, float, NewHealth, float, MaxHealth, float, Delta);

/** 死亡广播：生命首次归零时触发一次，KillerActor 为击杀者（未知则为 null） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnDeathSignature, AActor*, KillerActor);

/**
 * 竞技生化网游：生命状态组件（GAS 只读监听 + 事件转发）
 *
 * 架构定位：
 *  - 只读监听 GAS 生命属性（Health / MaxHealth）的变化，向 HUD / 伤害数字 / 受击表现广播；
 *  - 不承载任何权威伤害计算（伤害数值一律由 GAS GameplayEffect 在服务器结算）。
 *
 * 热更新边界：本组件只负责"把数值变化转成事件"，具体表现（血条动画、飘字、死亡演出）
 * 全部由 Lua 订阅下方事件后处理，改动表现无需重编译 C++。
 */
UCLASS()
class ZOMBIEKING_API UHealthComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UHealthComponent();

    /**
     * 与 GAS 能力系统建立订阅关系（在 PlayerState 的 ASC 就绪后由 AGameCharacterBase 调用）。
     * 可重复调用（重生/重连时）：内部会先解绑旧订阅，避免重复注册导致事件多次触发。
     */
    void InitializeWithAbilitySystem(UAbilitySystemComponent* InASC);

    // -------------------------------------------------------------------------
    // 对外事件（C++ / 蓝图 / UnLua 均可订阅）
    // -------------------------------------------------------------------------
    /** 生命值或生命上限发生变化时广播 */
    UPROPERTY(BlueprintAssignable, Category = "Health")
    FOnHealthChangedSignature OnHealthChanged;

    /** 生命首次归零时广播（去重，避免多次扣血/预测回滚导致死亡演出重复触发） */
    UPROPERTY(BlueprintAssignable, Category = "Health")
    FOnDeathSignature OnDeath;

    // -------------------------------------------------------------------------
    // 只读查询
    // -------------------------------------------------------------------------
    /** 当前生命值（未接入 ASC 时返回 0） */
    UFUNCTION(BlueprintPure, Category = "Health")
    float GetHealth() const;

    /** 当前生命上限（未接入 ASC 时返回 0） */
    UFUNCTION(BlueprintPure, Category = "Health")
    float GetMaxHealth() const;

    /** 生命百分比 0..1（供血条使用；上限为 0 时返回 0） */
    UFUNCTION(BlueprintPure, Category = "Health")
    float GetHealthPercent() const;

    /** 是否已死亡（生命 <= 0） */
    UFUNCTION(BlueprintPure, Category = "Health")
    bool IsDead() const;

protected:
    /** 组件销毁时解绑 ASC 委托，防止回调悬空 */
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** ASC 的 Health 属性变化回调 → 转发为项目事件 */
    void HandleHealthChanged(const FOnAttributeChangeData& Data);

    /** ASC 的 MaxHealth 属性变化回调 → 驱动 UI 重算百分比 */
    void HandleMaxHealthChanged(const FOnAttributeChangeData& Data);

    /** 解绑全部 ASC 属性订阅（重复初始化与 EndPlay 共用） */
    void UnbindAbilitySystemDelegates();

protected:
    /** 缓存的 GAS 组件（弱引用，避免生命周期悬空） */
    TWeakObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

    /** 属性变化委托句柄：EndPlay 时据此解绑 */
    FDelegateHandle HealthChangedDelegateHandle;
    FDelegateHandle MaxHealthChangedDelegateHandle;

    /** 死亡已广播的去重标志（复活/回血后重置） */
    bool bDeathBroadcast = false;
};
