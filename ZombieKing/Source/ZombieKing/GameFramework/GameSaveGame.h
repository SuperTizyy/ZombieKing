#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"   // USaveGame（本地存档对象基类）
#include "GameSaveGame.generated.h"

/**
 * 竞技生化网游：玩家档案存档对象（本地持久化）
 *
 * 定位与边界：
 *  - 本类承载**玩家档案**（身份显示信息、游玩时间等），与"设置"分开存放：
 *    设置是**每台机器**的（同一账号换机器通常要重设），档案是**每个玩家**的；
 *    混在一个存档里会出现"重置设置把档案也清了"这类问题。
 *  - 刻意只放**最小且确定**的字段：成长等级、解锁、战绩（K/D/A）等必须等玩法定稿后再设计，
 *    现在凭空定义会与后续设计冲突，且会污染存档结构（存档一旦发出去就要兼容）。
 *
 * 存档兼容纪律：任何字段变更都要**提升 SaveVersion 并写迁移逻辑**，
 *              否则老玩家读新存档会静默丢数据 —— 这是线上产品最容易踩的坑之一。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameSaveGame : public USaveGame
{
    GENERATED_BODY()

public:
    /** 存档版本号：字段结构变更时递增，并在读取时做迁移（从 1 开始） */
    UPROPERTY(BlueprintReadWrite, Category = "Version")
    int32 SaveVersion = 1;

    /** 玩家显示名（本地档案里的默认昵称；真正对外昵称以后端账号为准） */
    UPROPERTY(BlueprintReadWrite, Category = "Profile")
    FString PlayerDisplayName;

    /** 首次进入游戏的时间（用于"首次登录奖励/新手引导"这类判断） */
    UPROPERTY(BlueprintReadWrite, Category = "Profile")
    FDateTime FirstPlayTime;

    /** 最近一次进入游戏的时间（用于"回坑奖励/离线时长"这类判断） */
    UPROPERTY(BlueprintReadWrite, Category = "Profile")
    FDateTime LastPlayTime;
};
