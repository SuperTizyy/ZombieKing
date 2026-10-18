#pragma once

#include "CoreMinimal.h"
#include "GameCharacterBase.h"
#include "ZombieBasicCharacter.generated.h"

/**
 * 竞技生化网游：基础幽灵（变身选择流程的**免费保底角色**，即玩家仓库默认拥有的"普通母体"）
 *
 * 玩法定位（CF 生化模式惯例，用户拍板）：
 *  - 玩家被选拔为母体 / 被攻击感染时，会弹出"变身选择框"，从自己**仓库中拥有的幽灵角色**里选一个；
 *  - 选择框带倒计时，超时或未选择 → 自动变成本角色（人人免费拥有，保证流程永不卡死）；
 *  - 定位：中型双足幽灵——数量压力型（血量偏薄、移速偏快），与压制型母体（大体积高血）、
 *    机动型爬墙怪（小体积可爬墙）构成差异化横截面；
 *  - 具体数值（血量/移速/跳跃）不在 C++：血量走角色定义资产引用的初始化 GameplayEffect，
 *    移速/跳跃走角色定义资产的 WalkSpeed/JumpHeight 字段（改资产即生效）。
 *
 * 分层说明（与 AZombieCrawlerCharacter / AZombieMotherCharacter 同级，共用 AGameCharacterBase）：
 *  - 输入绑定/输入指令生产/GAS 托管等通用逻辑全部在基类，本类只做"形态声明 + 类默认值"；
 *  - 双足不可爬墙：只声明 Biped 能力标签，不换装爬墙运动组件（标签与实际能力必须一致）。
 */
UCLASS()
class ZOMBIEKING_API AZombieBasicCharacter : public AGameCharacterBase
{
    GENERATED_BODY()

public:
    AZombieBasicCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());
};
