#pragma once

#include "CoreMinimal.h"
#include "GameCharacterBase.h"
#include "ZombieCrawlerCharacter.generated.h"

class UCrawlerMoverComponent;

/**
 * 竞技生化模式：多足爬墙幽灵角色实体（UE5.7 Mover 2.0 预测管线版）
 *
 * 分层说明（通用逻辑在基类，多足专属在本类）：
 *  - 输入（移动/视角/跳跃/攻击的绑定与缓存）与输入指令生产已统一上收至 AGameCharacterBase，
 *    本类不再重复任何按键代码；
 *  - 通过 SetDefaultSubobjectClass 把基类的 MoverComponent 替换为 UCrawlerMoverComponent
 *    （多足专属运动组件：内部已组装 Climbing 爬墙模式 + 全局吸附过渡，详见其构造函数）；
 *  - 本类只保留多足专属职责：爬墙状态查询（IsClimbing）与"进出爬墙 → Lua 表现事件"的薄转发。
 */
UCLASS()
class ZOMBIEKING_API AZombieCrawlerCharacter : public AGameCharacterBase
{
    GENERATED_BODY()

public:
    AZombieCrawlerCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    /** 当前是否处于贴墙爬行状态（供 Lua/UI/音效系统查询） */
    UFUNCTION(BlueprintPure, Category = "Crawler")
    bool IsClimbing() const;

protected:
    virtual void BeginPlay() override;

    /**
     * 重写移动参数应用：先应用基类的行走速度/跳跃高度，再追加"爬墙速度"。
     * 说明：爬墙速度属于 UCrawlerClimbMode 的私有参数（不经过 Mover 共享设置），
     *       因此必须在本类中单独定位该模式实例后写入。
     */
    virtual void ApplyMovementParams() override;

    /**
     * Mover 运动模式变化回调（由 MoverComponent->OnMovementModeChanged 委托驱动）：
     * 进出 Climbing 时广播基类的 Lua 事件 OnClimbStateChanged，
     * Lua 层据此接管镜头 FOV/热成像后处理/音效/UI 等表现切换。
     */
    UFUNCTION()
    void HandleMovementModeChanged(const FName& PreviousModeName, const FName& NewModeName);
};
