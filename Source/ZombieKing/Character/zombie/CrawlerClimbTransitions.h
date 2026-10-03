#pragma once

#include "CoreMinimal.h"
#include "MovementModeTransition.h"
#include "CrawlerClimbTransitions.generated.h"

/**
 * 多足生化角色：墙面自动吸附过渡（Mover 全局过渡，Official Transition 机制）
 *
 * 触发条件（按玩法设计，全部在移动模拟内评估，服务器/客户端/重模拟完全一致）：
 *  1. 当前模式为 Walking 或 Falling（地面行走或空中下落，爬墙中/飞行中不重复评估）；
 *  2. 从角色中心沿"视线方向"射线检测命中可攀爬墙面（命中距离 <= AttachMaxDistance，即"合理距离"）；
 *  3. 命中面为"接近垂直的墙面"（坡面角度 >= MinClimbableSlopeAngle，天花板/缓坡地面不吸附）；
 *  4. 摄像机面向墙面：视线方向与墙面法线的方向要求满足 dot(ViewDir, -WallNormal) >= MinFacingWallDot，
 *     默认 0.5（即视线与墙面表面的夹角 >= 30°），落实"摄像机面向墙面且夹角达标才进入爬墙"的设计。
 *
 * 说明：
 *  - 视线方向取自输入指令中的 ControlRotation（随预测复制），AI 角色无相机时回退为角色前向；
 *  - 本过渡注册在 MoverComponent 的全局 Transitions 数组中（见 UCrawlerMoverComponent 构造函数）。
 */
UCLASS(BlueprintType, Blueprintable)
class ZOMBIEKING_API UCrawlerAttachTransition : public UBaseMovementModeTransition
{
    GENERATED_BODY()

public:
    /** 核心评估：返回进入爬墙模式的过渡结果（或 NoTransition） */
    virtual FTransitionEvalResult Evaluate_Implementation(const FSimulationTickParams& Params) const override;

public:
    // -------------------------------------------------------------------------
    // 吸附判定参数（EditDefaultsOnly：蓝图子类可调）
    // -------------------------------------------------------------------------
    /**
     * 吸附检测的最大距离（cm）：从角色中心沿视线方向射线的"合理距离"上限。
     * 参考值 = 胶囊半径（60）+ 一个身位的余量（60）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Attach")
    float AttachMaxDistance = 120.0f;

    /** 可攀爬墙面的最小坡面角度（法线与竖直方向的夹角，70° ≈ 接近垂直的墙体） */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Attach", meta = (ClampMin = "0", ClampMax = "90"))
    float MinClimbableSlopeAngle = 70.0f;

    /**
     * 视线面向墙面程度阈值：dot(视线方向, -墙面法线) 的最小值。
     * 0.5 = cos(60°)，等价于"视线与墙面表面的夹角 >= 30°"（垂直正面看墙时为 1.0）。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Attach", meta = (ClampMin = "-1", ClampMax = "1"))
    float MinFacingWallDot = 0.5f;

    /** 调试绘制：吸附检测射线与命中结果（仅非 Shipping 构建生效） */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Debug")
    bool bDrawDebugAttachTrace = false;
};
