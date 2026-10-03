#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "CombatComponent.generated.h"

/**
 * 竞技生化网游：战斗逻辑组件（占位空壳版）
 *
 * 架构定位：
 *  - 承载"受击硬直计算、部位判定（头/胸/四肢倍率）、反作弊验证回执"等战斗辅助逻辑；
 *  - 权威伤害数值仍由 GAS GameplayEffect 在服务器结算，本组件只做表现层与校验辅助。
 *
 * TODO（后续迭代补全）：
 *  - 部位判定：基于骨骼名映射伤害倍率（爆头/弱点部位），生化幽灵弱点部位特殊处理；
 *  - 受击硬直：多足爬墙角色受击时是否打断吸附（与 UCrawlerClimbMode 的联动）；
 *  - 反作弊：受击者回执校验（客户端上报 vs 服务器复算）；
 *  - 向 Lua 派发 OnDamaged 事件（当前 Lua 层 GameCharacterBase:OnDamaged_Implementation 已预留）。
 */
UCLASS()
class ZOMBIEKING_API UCombatComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UCombatComponent();
};
