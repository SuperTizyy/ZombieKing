#include "CrawlerMoverComponent.h"
#include "CrawlerClimbMode.h"
#include "CrawlerClimbTransitions.h"

UCrawlerMoverComponent::UCrawlerMoverComponent()
{
    // -------------------------------------------------------------------------
    // 1. 注册多足贴墙爬行模式
    //    - CreateDefaultSubobject 在组件构造函数中创建，模式实例自动归属本组件，
    //      满足 UBaseMovementMode 的 Within=MoverComponent 约束；
    //    - 引擎在 MoverComponent 初始化阶段读取 MovementModes 映射注册进 FSM。
    // -------------------------------------------------------------------------
    MovementModes.Add(CrawlerModeNames::Climbing, CreateDefaultSubobject<UCrawlerClimbMode>(TEXT("CrawlerClimbMode")));

    // -------------------------------------------------------------------------
    // 2. 注册全局吸附过渡（任何模式下持续评估，具体门控在 Evaluate 内部完成）
    //    触发条件：Walking/Falling + 视线射线命中合理距离内的近垂直墙面 + 视线与墙面夹角 >= 30°。
    // -------------------------------------------------------------------------
    Transitions.Add(CreateDefaultSubobject<UCrawlerAttachTransition>(TEXT("CrawlerAttachTransition")));

    // 说明：初始运动模式沿用引擎默认（Falling 出生 → 落地自动转 Walking），
    // 爬墙完全由吸附过渡触发，无需在角色 BeginPlay 中强制排队切换模式。
}
