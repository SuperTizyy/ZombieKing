#pragma once

#include "CoreMinimal.h"
#include "GameCharacterBase.h"
#include "HumanCharacter.generated.h"

/**
 * 竞技生化网游：人类幸存者角色
 *
 * 分层说明（严格对齐游戏定义）：
 *  - 人类与生化幽灵共用同一个公共基类 AGameCharacterBase（"共用一个角色基类"），
 *    本类只是它的一个普通子类，与 AZombieCrawlerCharacter 同级；
 *  - 人类仅双足、不能爬墙 —— 因此本类只声明 Biped 能力标签，且不挂载任何爬墙运动组件/模式；
 *  - "高矮胖瘦"由 AGameCharacterBase::BodyProfile 数据资产在运行期灌入，本类无需为每种体型再建类。
 */
UCLASS()
class ZOMBIEKING_API AHumanCharacter : public AGameCharacterBase
{
    GENERATED_BODY()

public:
    AHumanCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
