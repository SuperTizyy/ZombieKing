#pragma once

#include "CoreMinimal.h"
#include "MovementMode.h"
#include "MoveLibrary/MovementUtilsTypes.h"
#include "CrawlerClimbMode.generated.h"

class UPrimitiveComponent;

/**
 * 多足生化角色自定义运动模式名集合
 * 注册进 Mover 状态机时的唯一键（与蓝图/输入指令 SuggestedMovementMode 使用同一套名字）
 */
namespace CrawlerModeNames
{
    /** 贴墙爬行模式：仅对"接近垂直"的墙面生效（吸附过渡负责切入，本模式负责维持与退出） */
    const FName Climbing = TEXT("Climbing");
}

/**
 * 多足生化角色贴墙移动模式（UE5.7 Mover 2.0 范式重写版）
 *
 * 核心职责：
 *  - 沿角色腹部方向（局部 -Up）多发线探测墙面法线，将重力贴合到墙体表面，
 *    把移动输入投影到墙体切线平面，实现沿墙面自由爬行；
 *  - 维持"角色局部 Up = 墙面法线"的贴墙姿态（多足生物腹部紧贴墙面）；
 *  - 丢失表面 / 按下跳跃时，保留动量切回 Falling（蹬墙弹射），交由引擎默认管线落地。
 *
 * 网络与预测（竞技网游关键设计）：
 *  - 本模式的所有逻辑（射线检测、速度解算、位移执行）全部运行在 Mover 模拟管线内，
 *    在服务器、自主代理客户端、以及客户端重模拟（resim）中执行完全相同的代码路径；
 *  - 一切输入（相机朝向、移动意图、跳跃标志）均来自复制的输入指令（InputCmd），
 *    模式内部严禁读取任何仅在本地存在的状态（如 Controller、UI 状态）；
 *  - 模式切换通过 QueueNextMode 排队，随模拟在下一帧原子生效，保证预测一致。
 */
UCLASS(BlueprintType, Blueprintable)
class ZOMBIEKING_API UCrawlerClimbMode : public UBaseMovementMode
{
    GENERATED_BODY()

public:
    UCrawlerClimbMode();

    /**
     * 生成移动意图：把输入指令中的移动向量换算为世界空间期望速度。
     * 注意：切平面投影需要墙面法线，而法线探测放在 SimulationTick 中统一处理，
     * 这里只负责"原始期望速度"的换算（保持 GenerateMove 轻量、纯粹）。
     */
    virtual void GenerateMove_Implementation(const FMoverTickStartData& StartState, const FMoverTimeStep& TimeStep, FProposedMove& OutProposedMove) const override;

    /**
     * 核心物理模拟 Tick：由 MoverComponent 的网络预测管线在服务器与客户端自动平滑调用。
     * 流程：探测表面 → 速度解算（切线速度 + 吸附推力 + 可配滑落）→ 姿态对齐 → 安全位移 → 提交同步状态。
     */
    virtual void SimulationTick_Implementation(const FSimulationTickParams& Params, FMoverTickEndData& OutputState) override;

public:
    // -------------------------------------------------------------------------
    // 爬行参数（EditDefaultsOnly：蓝图子类/Datatable 可调，运行期不应随意修改）
    // -------------------------------------------------------------------------
    /** 爬行最大移动速度（cm/s） */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Movement")
    float MaxClimbSpeed = 800.0f;

    /** 沿腹部方向（局部 -Up）的表面探测距离（cm）。应大于胶囊半径 + 安全余量 */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Trace")
    float SurfaceTraceDistance = 150.0f;

    /** 多发线探测的偏移半径（cm）：在角色前/后/左/右各偏移此距离补射，增强凸边/凹角稳定性 */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Trace")
    float ProbeOffset = 40.0f;

    /** 吸附推力（cm/s）：持续把角色往墙面法线反方向压，保证越障/凸边时不脱墙 */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Movement")
    float SurfaceAdhesionStrength = 200.0f;

    /**
     * 抓握强度 0..1：
     *  - 1.0 = 完全吸附：无输入时静止在墙上（默认，符合生化幽灵"钉死在墙上"的压迫感）；
     *  - < 1.0 = 沿切平面重力分量带来缓慢下滑（0 为完全自由滑落）。
     * 说明：下滑速度按"重力切向分量 × (1 - GripStrength)"直接作为速度叠加，单位 cm/s。
     */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Movement", meta = (ClampMin = "0", ClampMax = "1"))
    float GripStrength = 1.0f;

    /** 此坡面角度之上才视为"可攀爬的墙面"（法线与水平面夹角，70° ≈ 接近垂直的墙体，天花板/大坡地面不可爬） */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Trace", meta = (ClampMin = "0", ClampMax = "90"))
    float MinClimbableSlopeAngle = 70.0f;

    /** 蹬墙弹射速度（cm/s）：沿墙面法线方向跃出 */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Jump")
    float JumpOffSpeed = 600.0f;

    /** 蹬墙弹射附加的向上提升速度（cm/s）：让弹射弧线更利于追击，可按玩法调为 0 */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Jump")
    float JumpOffUpSpeed = 200.0f;

    /** 调试绘制：爬行探测射线与命中法线（仅非 Shipping 构建生效） */
    UPROPERTY(EditDefaultsOnly, Category = "Crawler|Debug")
    bool bDrawDebugClimbTrace = false;

protected:
    /**
     * 表面探测：沿角色腹部方向（局部 -Up）从中心及四个偏移点发线检测，
     * 返回最近的有效命中（要求坡面角度 >= MinClimbableSlopeAngle，即近似垂直的墙面）。
     *
     * @param MovingComps         Mover 组件集合（提供被移动组件与碰撞参数来源）
     * @param OutSurfaceNormal    输出：墙面法线（世界空间，指向角色一侧）
     * @param OutSurfaceHitPoint  输出：命中点
     * @param OutHitDistance      输出：从组件中心到命中点的距离
     * @param OutHitResult        输出：完整命中结果（供黑板缓存/调试绘制）
     * @return 是否探测到合格的可攀爬表面
     */
    bool DetectSurfaceNormal(const FMovingComponentSet& MovingComps,
                             FVector& OutSurfaceNormal,
                             FVector& OutSurfaceHitPoint,
                             float& OutHitDistance,
                             FHitResult& OutHitResult) const;

    /** 判断一个表面法线是否满足"接近垂直可攀爬"的坡面要求 */
    bool IsClimbableSurface(const FVector& SurfaceNormal) const;
};
