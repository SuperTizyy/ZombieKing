#pragma once

#include "CoreMinimal.h"
#include "GameAnimInstanceBase.h"
#include "ZombieScorpionAnimInstance.generated.h"

class UBlendSpace;

/**
 * 蝎子（多足爬墙生化角色）：专属动画实例类
 *
 * 设计定位（重要架构约定）：
 *  - 多足生化角色"体态各不相同"，动画状态机与骨架无法像双足人类那样跨角色复用，
 *    因此每个体态拥有专属的动画实例类与动画蓝图（本类仅服务于蝎子）；
 *  - 共享层仅保留 UGameAnimInstanceBase（与体态无关的引擎数据桥：Mover 速度/
 *    方向/落地/模式名），不含任何动画表现逻辑；
 *  - 移动能力（爬墙模式/吸附过渡）由 UCrawlerMoverComponent 提供，与本类解耦：
 *    只能平地行走的多足角色既不需要爬墙槽位，也不用本类；
 *  - 新增其他体态（蜘蛛、蜈蚣等）= 各自新建专属动画实例类 + 专属动画蓝图，互不影响。
 *
 * 扩展内容：
 *  - bIsClimbing：Mover 当前模式 == CrawlerModeNames::Climbing（与 C++ 模式注册、
 *    输入指令使用同一套模式名，服务器/客户端/重模拟完全一致）；
 *  - ClimbBlendSpace：爬墙移动动画槽位（v1 复用爬行步态；后续接入墙面专用动画时，
 *    只需替换动画蓝图 Class Defaults 中的槽位资产，状态机零改动）。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UZombieScorpionAnimInstance : public UGameAnimInstanceBase
{
    GENERATED_BODY()

public:
    UZombieScorpionAnimInstance();

    virtual void NativeInitializeAnimation() override;

    /** 是否处于贴墙爬行状态（动画状态机 Climb 状态的唯一判定来源） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Scorpion")
    bool bIsClimbing = false;

    /** 爬墙移动混合空间槽位（2D：X=前后，Y=左右；未配置时爬墙移动状态无法正常播放）
     *  注意：UE5.7 起 BlendSpaceBase 已合并进 UBlendSpace，槽位统一用 UBlendSpace */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Slots")
    TObjectPtr<UBlendSpace> ClimbBlendSpace;

protected:
    /** 在通用运动数据基础上追加爬墙判定（每帧执行，纯数据、无 Lua 调用） */
    virtual void RefreshMovementState(float DeltaSeconds) override;
};
