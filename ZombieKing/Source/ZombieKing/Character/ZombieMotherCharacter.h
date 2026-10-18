#pragma once

#include "CoreMinimal.h"
#include "GameCharacterBase.h"
#include "ZombieMotherCharacter.generated.h"

/**
 * 竞技生化网游：母体（生化幽灵阵营的初始/核心个体）
 *
 * 分层说明（严格对齐游戏定义）：
 *  - 与人类共用同一个公共基类 AGameCharacterBase（"共用一个角色基类"），
 *    本类是它的一个普通子类，与 AZombieCrawlerCharacter（多足爬墙幽灵）同级；
 *  - 生化幽灵阵营内部形态多样（双足 / 多足 / 可爬墙），因此"运动能力标签"由各具体幽灵子类自行声明，
 *    本类暂不声明——待母体的形态定位确认后再补。
 */
UCLASS()
class ZOMBIEKING_API AZombieMotherCharacter : public AGameCharacterBase
{
    GENERATED_BODY()

public:
    AZombieMotherCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
