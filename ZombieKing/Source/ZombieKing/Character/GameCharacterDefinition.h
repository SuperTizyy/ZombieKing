#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameFaction.h"                   // EGameFaction 阵营枚举（独立轻量头，供 PlayerState/GameMode 共用）
#include "GameCharacterDefinition.generated.h"

class UGameCharacterBodyProfile;   // 体型档案（高矮胖瘦）
class UGameAbilitySet;             // 技能集
class UGameInputConfig;            // 技能输入配置（输入动作 → 技能标签）
class UGameplayEffect;             // 初始属性效果
class USkeletalMesh;               // 骨骼网格体（进化各级的"模型小变化"替换源）

/**
 * 生化幽灵单级进化数据（CF 式 6 级轨的"一级一行"数值表条目）。
 *
 * 语义约定（与角色侧落地逻辑严格对齐）：
 *  - 数组下标 0 = 1 级（初始级，MaxHealthDelta 应填 0 —— 1 级血量就是 InitialAttributeSetEffect 的基准值）；
 *  - MaxHealthDelta = "升到本级时"生命上限的**增量**（升级同时回等额血，见 ZombieEvolutionEffect 的 GE 定义）；
 *  - BodyScale = 本级的**绝对**体型缩放（作用于整个 Actor，碰撞与网格一起放大，各端由同一资产推导、无需复制）；
 *  - MeshOverride = 本级的模型替换（可空 = 沿用蓝图默认网格；"模型小变化"只换骨骼网格体，不换类）。
 */
USTRUCT(BlueprintType)
struct FGameZombieLevelEntry
{
    GENERATED_BODY()

    /** 升到本级新增的生命上限（1 级应填 0；本级及更高升级时按增量累加施加共享进化 GE） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ZombieLevel", meta = (ClampMin = "0.0"))
    float MaxHealthDelta = 0.0f;

    /** 本级绝对体型缩放（1.0 = 原始大小；逐级略增，如 1.0/1.05/1.1/...） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ZombieLevel", meta = (ClampMin = "0.1"))
    float BodyScale = 1.0f;

    /** 本级骨骼网格体替换（可空 = 不替换；同一骨架的"模型小变化"，如肌肉膨胀版） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ZombieLevel")
    TSoftObjectPtr<USkeletalMesh> MeshOverride;
};

/**
 * 竞技生化网游：角色定义（Character Definition，即大厂常说的 "PawnData"）
 *
 * 定位（对齐 UE 官方 Lyra 的 ULyraPawnData 范式）：
 *  - "一个角色类型 = 一份本资产"：体型、阵营、技能集、初始属性效果、移动参数全部在此声明；
 *  - 角色类（AGameCharacterBase 的子类）只负责"结构性能力"（双足/多足/是否挂爬墙 Mover 组件），
 *    数值与配置一律来自本资产；
 *  - 因此"高矮胖瘦 / 血量 / 跳跃高度 / 行走速度 / 技能集"的调整全部是内容改动，无需重编译 C++。
 *
 * 对应游戏定义（每只生化幽灵各不相同）：
 *  体型 → BodyProfile；血量 → InitialAttributeSetEffect；技能 → AbilitySets；
 *  跳跃高度 / 行走速度 / 爬墙速度 → 下方移动参数。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameCharacterDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    // =========================================================================
    //  身份
    // =========================================================================

    /** 显示名（便于策划在资产浏览器中辨识，不参与任何逻辑） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
    FText DisplayName;

    /** 阵营：决定敌我判定、属性集选择与初始效果（人类 / 生化幽灵） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
    EGameFaction Faction = EGameFaction::None;

    // =========================================================================
    //  体型（高矮胖瘦）
    // =========================================================================

    /**
     * 体型档案（数据资产）：角色的碰撞尺寸与表现缩放。
     * 由 AGameCharacterBase::ApplyBodyProfile 在 BeginPlay 灌入碰撞体与网格体。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Body")
    TObjectPtr<UGameCharacterBodyProfile> BodyProfile;

    // =========================================================================
    //  属性（血量等）
    // =========================================================================

    /**
     * 初始属性 GameplayEffect：在服务器灌入该角色类型的初始生命等数值。
     * "每只生化幽灵血量各不相同"即通过"每种类型一份 GE 资产"实现——改血量只改资产。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Attributes")
    TSubclassOf<UGameplayEffect> InitialAttributeSetEffect;

    // =========================================================================
    //  生化幽灵进化（CF 式 6 级轨；仅幽灵阵营使用，人类角色留空即可）
    // =========================================================================

    /**
     * 逐级进化数据表：下标 0 = 1 级，最多 6 项（级数上限是规则，Lua 判定；本表只承载"每级长什么样"）。
     * 未配置或等级超出表长：只应用已知范围内的数值，缺失级按"无增量/无缩放/不换模型"降级，绝不崩。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ZombieEvolution")
    TArray<FGameZombieLevelEntry> ZombieLevels;

    /**
     * 共享进化 GameplayEffect（全项目一份即可）：Instant，
     * MaxHealth += SetByCaller(Data.Zombie.MaxHealthDelta)，Health += 同值（升级即回等额血）。
     * 未配置 = 升级只改体型/模型不改血量（内容侧漏配时角色侧打告警不崩）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "ZombieEvolution")
    TSubclassOf<UGameplayEffect> ZombieEvolutionEffect;

    // =========================================================================
    //  技能（每只生化幽灵技能不同）
    // =========================================================================

    /**
     * 技能集列表：可挂多套便于复用；授予时由角色展平为技能列表交给 PlayerState。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
    TArray<TObjectPtr<UGameAbilitySet>> AbilitySets;

    /**
     * 技能输入配置（输入动作 → 技能标签）：
     * 角色据此把"按键"与"技能"解耦挂接；新增/更换技能输入只改本资产，无需重编译。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Abilities")
    TObjectPtr<UGameInputConfig> InputConfig;

    // =========================================================================
    //  移动参数（逐类型不同）
    // =========================================================================

    /** 行走速度（cm/s） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = "0.0"))
    float WalkSpeed = 600.0f;

    /** 跳跃高度（cm） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = "0.0"))
    float JumpHeight = 150.0f;

    /** 爬墙速度（cm/s）：仅"可爬墙的多足生化幽灵"有效，其余角色忽略此项 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = "0.0"))
    float ClimbSpeed = 800.0f;

    // -------------------------------------------------------------------------
    // TODO（后续里程碑，需单独设计后实现，本轮刻意不做以免过度设计）：
    //   把 WalkSpeed / JumpHeight / ClimbSpeed 接入 Mover 运动模式：
    //   Mover 的模式参数目前是在"组件构造期"以类默认值组装（见 UCrawlerMoverComponent /
    //   UCrawlerClimbMode），运行期改写需要在角色 BeginPlay 中定位到对应 MovementMode 实例后写入，
    //   并需验证其在预测/回滚管线中的一致性与复制行为，故留待专门的"移动参数接线"里程碑。
    // -------------------------------------------------------------------------
};
