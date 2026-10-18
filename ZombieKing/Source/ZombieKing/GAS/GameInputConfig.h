#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"     // FGameplayTag（输入标签）
#include "GameInputConfig.generated.h"

class UInputAction;

/**
 * 单条"输入 → 技能标签"映射
 * 说明：把"哪个按键"与"激活哪一类技能"解耦——按键绑定是输入层的事，技能是 GAS 层的事，
 *       两者通过 GameplayTag 对齐，因此换按键不需要改技能、换技能也不需要改按键。
 */
USTRUCT(BlueprintType)
struct FGameInputAction
{
    GENERATED_BODY()

    /** 输入动作（Enhanced Input 资产，如 IA_Skill1） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> InputAction;

    /**
     * 该输入对应的技能标签。
     * 约定：被激活的技能需在其 AbilityTags 中包含此标签，
     *       激活时通过 ASC->TryActivateAbilitiesByTag 按标签批量激活（引擎内建能力）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input", meta = (Categories = "Ability.Input"))
    FGameplayTag InputTag;
};

/**
 * 竞技生化网游：技能输入配置（Input Config）
 *
 * 大厂做法（对齐 UE 官方 Lyra 的 ULyraInputConfig）：
 *  - "输入动作 → 输入标签"的映射做成独立数据资产，由角色定义（UGameCharacterDefinition）引用；
 *  - 角色只负责"把配置里的绑定挂上去"，不关心具体是哪些技能；
 *  - 新增/更换技能输入只需改本资产，无需重编译 C++（满足竞技网游热更新要求）。
 *
 * 使用方式：
 *  1) 在本资产里配置若干条"输入动作 → 输入标签"；
 *  2) 技能（UGameGameplayAbility 的蓝图子类）在 Class Defaults → AbilityTags 中加入对应标签；
 *  3) 角色自动绑定：按下输入 → 按标签激活技能（见 AGameCharacterBase::BindAbilityInputActions）。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameInputConfig : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    /** 输入动作与技能标签的映射列表 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TArray<FGameInputAction> InputActions;
};
