#pragma once

#include "CoreMinimal.h"
#include "DefaultMovementSet/CharacterMoverComponent.h"
#include "CrawlerMoverComponent.generated.h"

/**
 * 多足生化角色专属 Mover 运动组件
 *
 * 设计说明（对齐引擎官方范式，参 UCharacterMoverComponent / UChaosWalkingMode 的组装方式）：
 *  - 运动模式实例必须归属 MoverComponent（UBaseMovementMode 有 Within=MoverComponent 约束），
 *    因此在组件构造函数中使用 CreateDefaultSubobject 创建模式与过渡，
 *    由引擎在 FSM 初始化时统一注册，天然满足归属约束且与 CDO/实例化流程兼容；
 *  - 角色的 BeginPlay 阶段无需再做任何运动模式注册，避免"注册时机晚于 FSM 初始化"的坑。
 *
 * 组装内容：
 *  - Climbing 模式（UCrawlerClimbMode）：多足贴墙爬行；
 *  - 全局吸附过渡（UCrawlerAttachTransition）：Walking/Falling 下自动切爬墙；
 *  - 其余能力（Walking/Falling/Flying、跳跃、落地检测）继承自 UCharacterMoverComponent。
 */
UCLASS()
class ZOMBIEKING_API UCrawlerMoverComponent : public UCharacterMoverComponent
{
    GENERATED_BODY()

public:
    UCrawlerMoverComponent();
};
