#pragma once

#include "CoreMinimal.h"
#include "GameCharacterBase.h"
#include "ZombieCrawlerCharacter.generated.h"

class UInputMappingContext;
class UInputAction;
class UCrawlerMoverComponent;

/**
 * 竞技生化模式：多足爬墙幽灵角色实体（UE5.7 Mover 2.0 预测管线版）
 *
 * 核心特性：
 *  - 扁平/矮粗胶囊体，适配多足蜘蛛/异形底盘；
 *  - 通过 SetDefaultSubobjectClass 把基类的 MoverComponent 替换为 UCrawlerMoverComponent
 *    （多足专属运动组件：内部已组装 Climbing 爬墙模式 + 全局吸附过渡，详见其构造函数）；
 *  - 吸附判定完全走 Mover 过渡机制（位于移动模拟内，服务器/客户端/重模拟一致），
 *    角色本类只负责"输入缓存 → 输入指令"与"模式变化 → Lua 事件"的薄转发；
 *  - Enhanced Input 回调统一转发到基类定义的 Lua 事件契约（OnInputMove/OnInputAttack 等）。
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
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
    virtual void OnProduceInput(float DeltaMs, FMoverInputCmdContext& OutInputCmd) override;

    // -------------------------------------------------------------------------
    // 输入动作资产配置（Enhanced Input，在蓝图子类 BP_ZombieCrawlerCharacter 中指定）
    // -------------------------------------------------------------------------
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputMappingContext> CrawlerMappingContext;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionMove;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionLook;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionAttack;

    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionJump;

    // -------------------------------------------------------------------------
    // C++ 输入回调：只做"缓存输入 + 转发 Lua 事件"，移动计算全部交给 Mover 模拟
    // -------------------------------------------------------------------------
    /** 移动输入：缓存为 -1..1 的本地轴意图（世界空间换算在 OnProduceInput 按相机朝向完成） */
    void Input_Move(const struct FInputActionValue& Value);

    /** 视角输入：直接驱动 Controller 旋转（相机世界坐标方案，爬墙时镜头不翻转） */
    void Input_Look(const struct FInputActionValue& Value);

    /** 攻击输入：转发 Lua（GAS 技能激活请求 + 第一人称攻击表现） */
    void Input_Attack();

    /** 跳跃输入：置位跳跃标志（模拟帧消费）+ 转发 Lua 表现 */
    void Input_Jump();

    /** 跳跃释放：清除持续按压标志 */
    void Input_JumpReleased();

    /**
     * Mover 运动模式变化回调（由 MoverComponent->OnMovementModeChanged 委托驱动）：
     * 进出 Climbing 时广播基类的 Lua 事件 OnClimbStateChanged，
     * Lua 层据此接管镜头 FOV/热成像后处理/音效/UI 等表现切换。
     */
    UFUNCTION()
    void HandleMovementModeChanged(const FName& PreviousModeName, const FName& NewModeName);
};
