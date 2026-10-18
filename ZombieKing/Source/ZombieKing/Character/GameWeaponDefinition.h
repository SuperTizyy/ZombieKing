#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"    // FGameplayTagContainer（武器标签 + 音频事件标签）
#include "GameWeaponTypes.h"         // EGameWeaponFireMode / EGameWeaponPayloadKind
#include "GameWeaponDefinition.generated.h"

class UGameplayEffect;   // 伤害效果类（需引用 UGameDamageExecution）
class AGameProjectile;   // 投射物 Actor 类（PayloadKind = Projectile 时使用）
class USkeletalMesh;     // 第一人称武器模型（内容侧资产）

/**
 * 竞技生化网游：武器定义（一个武器类型 = 一份本资产）
 *
 * 定位（与 UGameCharacterDefinition 同一套"数据驱动 = 内容改动无需重编译"的思路）：
 *  - 一把枪的**全部数值**集中在本资产：射速、弹匣、换弹时长、伤害、散布、射程、投射物参数、
 *    表现参数（后坐力/挂点/音频事件标签）。调枪感 = 改资产，不碰 C++；
 *  - **机制**（射击结算流程、弹药扣减、换弹状态机、切枪、反作弊节流）在
 *    UGameWeaponComponent（服务器权威）里，本资产只被读取、不含逻辑。
 *
 * 与技能的关系：
 *  - 开火入口仍是 GAS 技能（UGameRangedAttackAbility）：输入标签 → 技能激活 → 委托武器组件执行；
 *    这样"被定身/眩晕时不能开枪"这类阻断可以复用 GAS 的技能阻断能力；
 *  - 但"射速节流 / 弹药 / 换弹 / 切枪"这些**武器实例状态**全在组件里，
 *    避免散落到多个技能中导致状态机打架。
 *
 * 内容侧使用：
 *  1) 用本类建数据资产（如 DA_Weapon_Rifle），填入数值与表现参数；
 *  2) 把资产填进角色的 UGameCharacterDefinition::DefaultWeaponLoadout（或武器组件的装备槽）；
 *  3) 若为霰弹枪：PelletCount > 1 + SpreadAngleDegrees > 0；若为投射物武器：PayloadKind = Projectile 并填 ProjectileClass。
 */
UCLASS(BlueprintType)
class ZOMBIEKING_API UGameWeaponDefinition : public UPrimaryDataAsset
{
    GENERATED_BODY()

public:
    UGameWeaponDefinition();

    /**
     * 主资产标识覆写（必须）：
     * `UPrimaryDataAsset` 默认用"最上层原生类名"（即 `GameWeaponDefinition`）当类型，
     * 与内容管线的登记类型对不上；这里固定为 `WeaponDefinition`，口径统一、便于按类型批量查询。
     */
    virtual FPrimaryAssetId GetPrimaryAssetId() const override;

    // =========================================================================
    //  身份
    // =========================================================================

    /** 显示名（便于策划在资产浏览器与枪械配置表中辨识，不参与逻辑） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
    FText DisplayName;

    /**
     * 武器标签（内容侧自定义，如 Weapon.Slot.Primary / Weapon.Type.Rifle）。
     * 用途：给 Lua 规则、AI、HUD 提供统一的分类查询口径（例如"只允许切到主武器槽"）。
     * 说明：刻意不做成 C++ 原生标签 —— 分类维度会随玩法增长，用资产标签更灵活。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Identity")
    FGameplayTagContainer WeaponTags;

    // =========================================================================
    //  开火与弹道
    // =========================================================================

    /** 开火模式：单发 / 全自动（仅影响"能否按住连发"的输入语义，射速一律由组件节流裁决） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire")
    EGameWeaponFireMode FireMode = EGameWeaponFireMode::Auto;

    /** 弹道结算方式：即时射线 / 投射物 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire")
    EGameWeaponPayloadKind PayloadKind = EGameWeaponPayloadKind::Hitscan;

    /** 每分钟射速（发/分钟）：组件据此换算最小开火间隔做服务器权威节流。0 表示不限制 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire", meta = (ClampMin = "0.0", ClampMax = "3000.0"))
    float RoundsPerMinute = 600.0f;

    /**
     * 单次开火的弹丸数：1 = 普通枪械；>1 = 霰弹（一次开火发射多颗，每颗独立判定与结算）。
     * 注意：霰弹的多颗是"同一次开火内的多条射线"，因此并不增加输入开销，但服务器要做 N 次射线检测。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire", meta = (ClampMin = "1", ClampMax = "64"))
    int32 PelletCount = 1;

    /**
     * 散布角（度，锥体全角）：0 = 零散布（竞技精准射击）。
     * 霰弹枪的"扩散"与自动武器的"腰射散布"都靠它表达；属于手感数值，改资产即生效。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire", meta = (ClampMin = "0.0", ClampMax = "180.0"))
    float SpreadAngleDegrees = 0.0f;

    /** 有效射程（cm）：射线的最大长度，超出即视为未命中（投射物武器不使用本项） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire", meta = (ClampMin = "0.0"))
    float TraceRange = 12000.0f;

    /**
     * 射线追踪通道：默认 ECC_Pawn。
     * 原因（引擎事实）：角色胶囊体默认使用 "Pawn" 碰撞配置，该配置显式忽略了 Visibility 通道，
     * 用 Visibility 将打不到角色；ECC_Pawn 同时能被墙体（BlockAll）阻挡，是 hitscan 的稳妥默认。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Fire")
    TEnumAsByte<ECollisionChannel> TraceChannel = ECC_Pawn;

    // =========================================================================
    //  伤害
    // =========================================================================

    /** 单颗弹丸的原始伤害值（正数）；霰弹枪的实际总伤害 ≈ 本值 × PelletCount */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = "0.0"))
    float DamagePerPellet = 20.0f;

    /**
     * 伤害效果类：需引用 UGameDamageExecution 作为执行计算（同一份 GE 可服务所有武器，
     * 因为伤害数值通过 SetByCaller(Data.Damage) 传入）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
    TSubclassOf<UGameplayEffect> DamageEffectClass;

    // =========================================================================
    //  弹药与换弹
    // =========================================================================

    /** 弹匣容量（发）：打空后需换弹 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = "1"))
    int32 MagazineSize = 30;

    /** 换弹时长（秒）：服务器权威计时，完成后从备弹移入弹匣；改此值即调整换弹手感 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = "0.0", ClampMax = "30.0"))
    float ReloadSeconds = 2.4f;

    /** 出生/补给时发放的备弹量（不含已装填的弹匣） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = "0"))
    int32 StartingReserveAmmo = 90;

    /** 备弹上限：拾取补给时不能超过此值 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = "0"))
    int32 MaxReserveAmmo = 180;

    /**
     * 打空弹匣后是否自动换弹（仅当备弹 > 0）。
     * 默认关闭：竞技射击游戏通常把换弹交给玩家决策（"打空后是否换弹"是战术选择），
     * 需要"新手向"的武器再打开本项。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Ammo")
    bool bAutoReloadWhenEmpty = false;

    // =========================================================================
    //  投射物参数（仅 PayloadKind = Projectile 时使用）
    // =========================================================================

    /** 投射物 Actor 类（须为 AGameProjectile 的子类；内容侧用蓝图派生以挂表现） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile")
    TSubclassOf<AGameProjectile> ProjectileClass;

    /** 投射物初速（cm/s） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = "0.0"))
    float ProjectileInitialSpeed = 4500.0f;

    /** 投射物重力缩放（0 = 无重力直线飞行；1 = 完整重力，用于投掷物抛物线） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = "0.0", ClampMax = "4.0"))
    float ProjectileGravityScale = 0.0f;

    /** 投射物存活上限（秒）：超时未命中即销毁（防止"漏网"的投射物永久留在场景里） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = "0.1", ClampMax = "60.0"))
    float ProjectileLifeSeconds = 6.0f;

    // =========================================================================
    //  表现参数（供 Lua 读取；本类不主动播放任何表现，保持"表现下沉 Lua"）
    // =========================================================================

    /** 开火镜头后坐力 - 俯仰角（度）：Lua 用它在本地做镜头抖动（各端表现，不参与判定） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "20.0"))
    float RecoilPitchKick = 0.6f;

    /** 开火镜头后坐力 - 偏航角（度）：Lua 用它在本地做随机水平抖动 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation", meta = (ClampMin = "0.0", ClampMax = "20.0"))
    float RecoilYawKick = 0.15f;

    /** 第一人称武器模型（软引用；由内容/Lua 挂到角色的武器挂点上，不做成网络 Actor） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
    TSoftObjectPtr<USkeletalMesh> FirstPersonWeaponMesh;

    /** 武器模型挂接的骨骼挂点名（在第一人称手臂/枪械骨架上） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
    FName WeaponMeshAttachSocket = FName("weapon_r");

    /** 枪口挂点名（枪口火焰、投射物生成起点、曳光弹起点都用它） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation")
    FName MuzzleSocketName = FName("Muzzle");

    /**
     * 音频事件标签（GameplayTag 驱动的音频事件表：玩法只报标签，音频资源由音频层查表决定）。
     * 为什么用标签而不是直接引用 SoundBase：武器资源替换（甚至按平台/语言换音）不应动到玩法侧数据，
     * 且标签可被音频层统一做混音分类（如挂到"武器"SoundClass）与并发控制。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Audio")
    FGameplayTag FireSoundEventTag;

    /** 换弹音效事件标签 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Audio")
    FGameplayTag ReloadSoundEventTag;

    /** 空仓扣扳机音效事件标签（打空弹匣时的"咔"声） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Presentation|Audio")
    FGameplayTag EmptySoundEventTag;

    // =========================================================================
    //  查询接口
    // =========================================================================

    /** 两次开火之间的最小间隔（秒）；RoundsPerMinute <= 0 视为"不限制"返回 0 */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    float GetFireIntervalSeconds() const;

    /** 伤害效果类（供武器组件 / 投射物读取） */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    TSubclassOf<UGameplayEffect> GetDamageEffectClass() const { return DamageEffectClass; }
};
