#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameAbilitySet.generated.h"

class UGameplayAbility;

/**
 * 竞技生化网游：技能集（Ability Set）
 *
 * 大厂做法（对齐 UE 官方 Lyra 的 ULyraAbilitySet 范式）：
 *  - 把"一组需要一起授予的技能"封装成一个可复用的数据资产；
 *  - 角色定义（UGameCharacterDefinition）只需引用若干技能集，而不必逐个罗列技能；
 *  - 同一套技能集可被多个角色类型复用（例如通用的"跳跃/冲刺"技能集）；
 *  - 改技能 = 改内容资产，无需重新编译 C++（满足竞技网游热更新要求）。
 *
 * 当前范围：只承载"授予技能"；后续可扩展被动 GameplayEffect 授予、属性集授予等。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameAbilitySet : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /**
     * 本技能集包含的技能列表。
     * 授予时由 AGamePlayerState 逐个 GiveAbility，并在离身/换人时按句柄精确清除。
     * 元素类型为技能基类（UGameplayAbility），因此既可填项目技能基类 UGameGameplayAbility 的子类，
     * 也可填任意第三方技能类。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "AbilitySet")
    TArray<TSubclassOf<UGameplayAbility>> GrantedAbilities;

    // -------------------------------------------------------------------------
    // TODO（后续扩展，对齐 Lyra 的 AbilitySet 结构，暂不实现以免过度设计）：
    //  - GrantedGameplayEffects：随技能集一起施加的被动效果（如常驻增益/减益）；
    //  - GrantedAttributeSet 相关：按技能集授予属性集的场景。
    // -------------------------------------------------------------------------
};
