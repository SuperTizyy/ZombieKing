#include "GamePlayerState.h"
#include "AbilitySystemComponent.h"

AGamePlayerState::AGamePlayerState()
{
    // 长期存活对象托管 GAS：ASC 跟随 PlayerState 复制与存活周期
    AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
    AbilitySystemComponent->SetIsReplicated(true);

    // TODO: 迭代时按需切换为 Mixed 复制模式，并将 Owner 的代理复制开启
    // AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

    // 默认无阵营：由 GameMode 在玩家进入/变异时通过 SetGenericTeamId 指定
    TeamId = FGenericTeamId::NoTeam;
}

UAbilitySystemComponent* AGamePlayerState::GetAbilitySystemComponent() const
{
    return AbilitySystemComponent;
}

void AGamePlayerState::SetGenericTeamId(const FGenericTeamId& InTeamID)
{
    TeamId = InTeamID;
}

FGenericTeamId AGamePlayerState::GetGenericTeamId() const
{
    return TeamId;
}
