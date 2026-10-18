#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameCharacterBodyProfile.generated.h"

/**
 * 角色碰撞形态枚举（高矮胖瘦的"底盘形状"）
 *
 * 关键约束（由引擎行为决定，不是设计选择）：
 *  - Mover 的 UpdatedComponent 取自 Actor 的 RootComponent，因此碰撞形态必须在
 *    "角色类构造期"就定好，无法在运行期按数据切换根组件；
 *  - 因此本枚举是"声明式"的：它声明该体型档案期望的形态，角色类必须已经用对应的根碰撞体
 *    （胶囊 or SetBoxCollisionRoot 切换出的盒体），二者不一致时 ApplyBodyProfile 会告警。
 */
UENUM(BlueprintType)
enum class ECharacterCollisionForm : uint8
{
    /** 胶囊体：通用双足体型（人类与多数生化幽灵），受引擎约束"半高 >= 半径" */
    Capsule UMETA(DisplayName = "Capsule (胶囊体 / 通用体型)"),

    /** 盒体：宽扁体型底盘（如"矮胖"幽灵），需子类在构造期调用 SetBoxCollisionRoot 切换根组件 */
    Box UMETA(DisplayName = "Box (盒体 / 宽扁体型)"),

    /** 枚举哨兵：用于数组容量与越界校验，绝不作为真实形态使用 */
    MAX UMETA(Hidden)
};

/**
 * 竞技生化网游：角色体型档案（Primary Data Asset）
 *
 * 定位：
 *  - 一个资产对应"一档体型"（如 矮/中/高 与 瘦/胖），由角色蓝图在 AGameCharacterBase::BodyProfile 引用；
 *  - 只承载"尺寸数值"，不含任何逻辑；运行期由 AGameCharacterBase::ApplyBodyProfile 灌入碰撞体与网格体。
 *
 * 为什么用 DataAsset 而不是 DataTable / 蓝图默认值（大厂惯例，对齐 UE 官方 Lyra 的角色定义方式）：
 *  - 类型安全、可被蓝图引用、可在内容热更中直接替换数值——改体型不需要重新编译 C++；
 *  - DataTable 更适合"扁平大数值表"（如武器伤害表），不适合承载带引用关系的角色定义。
 *
 * 热更新边界：本资产的全部字段都是"可热更数值"，符合竞技网游不停机调参要求。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameCharacterBodyProfile : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // =========================================================================
    //  标识
    // =========================================================================

    /** 体型唯一标识（如 Short_Tall / Normal / Fat_Wide），供日志与热更定位；仅作可读性用途 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body")
    FName ProfileId;

    // =========================================================================
    //  碰撞形态与尺寸
    // =========================================================================

    /**
     * 碰撞形态声明：必须与角色类实际使用的根碰撞体一致。
     * 不一致时 ApplyBodyProfile 会打出告警并跳过灌入（属于配置错误）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body")
    ECharacterCollisionForm CollisionForm = ECharacterCollisionForm::Capsule;

    // -------------------------------------------------------------------------
    //  胶囊形态尺寸（CollisionForm == Capsule 时生效）
    // -------------------------------------------------------------------------

    /** 胶囊半径（cm）。引擎硬约束：最终生效的半高会被钳制为 >= 半径 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body|Capsule", meta = (ClampMin = "1.0", UIMin = "1.0"))
    float CapsuleRadius = 40.0f;

    /** 胶囊半高（cm）。若小于半径，ApplyBodyProfile 会自动钳制为等于半径并告警 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body|Capsule", meta = (ClampMin = "1.0", UIMin = "1.0"))
    float CapsuleHalfHeight = 90.0f;

    // -------------------------------------------------------------------------
    //  盒体形态尺寸（CollisionForm == Box 时生效）
    // -------------------------------------------------------------------------

    /** 盒体半尺寸（X=前后, Y=左右, Z=上下，单位 cm）。用宽 Y、低 Z 即可表达"矮胖"宽扁底盘 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body|Box")
    FVector BoxHalfExtent = FVector(40.0f, 40.0f, 45.0f);

    // =========================================================================
    //  表现（让"高矮胖瘦"同时体现在视觉上）
    // =========================================================================

    /** 表现网格体统一缩放：碰撞由上方尺寸负责，视觉体积由本缩放表达 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body|Presentation")
    FVector MeshScale = FVector::OneVector;
};
