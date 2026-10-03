#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "GenericTeamAgentInterface.h"
#include "GamePlayerState.generated.h"

class UAbilitySystemComponent;

/**
 * 竞技生化网游：玩家状态类（占位空壳版）
 *
 * 架构定位（工业级 GAS 规范）：
 *  - PlayerState 是跨死亡/变异存活的长期对象，作为 GAS 的 OwnerActor 托管 AbilitySystemComponent；
 *  - 角色的 Pawn（AGameCharacterBase）仅作为短生命周期的 AvatarActor，
 *    感染变异/换人时只需切换 Avatar，不重建技能体系（避免 GAS 状态丢失与回调悬空）。
 *
 * TODO（后续迭代补全）：
 *  - 配置 ASC 的复制模式（Mixed）与 ReplicationProxy（Owner 是否可见自身属性）
 *  - 挂载 UAttributeSet（生命/护甲/移速等属性集）并绑定属性变化委托
 *  - 阵营（TeamId）驱动准星变色、友伤过滤（IGenericTeamAgentInterface 已实现）
 *  - 技能授予（给予初始技能、绑定 Enhanced Input 技能输入 Tag）
 */
UCLASS()
class ZOMBIEKING_API AGamePlayerState : public APlayerState,
                                         public IAbilitySystemInterface,
                                         public IGenericTeamAgentInterface
{
    GENERATED_BODY()

public:
    AGamePlayerState();

    // -------------------------------------------------------------------------
    // IAbilitySystemInterface
    // -------------------------------------------------------------------------
    /** 返回本 PlayerState 托管的 GAS 能力系统组件（占位实现，链路已通） */
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

    // -------------------------------------------------------------------------
    // IGenericTeamAgentInterface（阵营唯一数据源）
    // -------------------------------------------------------------------------
    virtual void SetGenericTeamId(const FGenericTeamId& InTeamID) override;
    virtual FGenericTeamId GetGenericTeamId() const override;

protected:
    /** GAS 能力系统组件（当前为空壳实例，后续挂 AttributeSet 与技能） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
    TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

    /** 阵营 ID：人类与生化幽灵分属不同阵营，驱动敌我判定 */
    FGenericTeamId TeamId;
};
