#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameProjectile.generated.h"

class USphereComponent;             // 碰撞球（根组件：投射物位移与命中的实际载体）
class UStaticMeshComponent;         // 表现网格（内容侧在蓝图子类里指定模型）
class UProjectileMovementComponent; // 投射物运动（初速/重力/扫掠碰撞）
class UGameplayEffect;              // 伤害效果类（需引用 UGameDamageExecution）
class UGameAbilitySystemComponent;  // 施加方 ASC（服务器结算伤害用）

/**
 * 竞技生化网游：投射物（RPG / 榴弹 / 投掷物等"飞行一段距离再命中"的武器弹体）
 *
 * 权威模型（服务器权威 + 客户端表现）：
 *  - **服务器**：唯一生成并初始化投射物（伤害/初速/重力/存活上限），命中时结算伤害，命中即销毁；
 *  - **客户端**：由复制获得投射物的位置与速度（纯表现），**不参与任何伤害结算**；
 *    命中特效不由此类负责 —— 走已有的 GameplayCue 通道（伤害 GE 携带 Cue 标签，
 *    服务端执行会 multicast 到相关客户端，保证各端表现一致且不会重复播放）。
 *
 * 为什么投射物必须是 Actor（而枪械不是）：
 *  - 投射物有**独立的、需要被所有人看到并随时间演变的空间位置**，属于世界内的实体，
 *    必须走 Actor 复制；而枪械只是持用者的一个状态（见 UGameWeaponComponent 的说明）。
 *
 * 生命周期：
 *  - 生成后若在 ProjectileLifeSeconds 内未命中，由 SetLifeSpan 自动销毁，
 *    避免"漏网的投射物"永久留在场景中（服务器销毁会复制到所有客户端）。
 */
UCLASS(Blueprintable)
class ZOMBIEKING_API AGameProjectile : public AActor
{
    GENERATED_BODY()

public:
    AGameProjectile();

    /**
     * 服务器初始化（发射时由武器组件调用；客户端调用会被安全忽略）。
     *
     * @param InDamageSource    开火者（伤害归因来源；同时用于命中时忽略自身）
     * @param InDamage          命中伤害（原始值；由伤害 GE 的 UGameDamageExecution 结算）
     * @param InDamageEffect    伤害效果类
     * @param InInitialSpeed    初速（cm/s）
     * @param InGravityScale    重力缩放（0 = 直线飞行）
     * @param InLifeSeconds     存活上限（秒）
     */
    void InitializeProjectile(AActor* InDamageSource, float InDamage, TSubclassOf<UGameplayEffect> InDamageEffect,
                              float InInitialSpeed, float InGravityScale, float InLifeSeconds);

protected:
    virtual void BeginPlay() override;

    /**
     * 命中回调（服务器结算）。
     * 签名必须与引擎的 FComponentHitSignature 完全一致（五参数稀疏动态委托），否则 AddDynamic 编译不过。
     */
    UFUNCTION()
    void HandleComponentHit(UPrimitiveComponent* HitComponent, AActor* OtherActor, UPrimitiveComponent* OtherComponent,
                            FVector NormalImpulse, const FHitResult& Hit);

    // -------------------------------------------------------------------------
    // 组件
    // -------------------------------------------------------------------------
    /** 碰撞球（根组件）：投射物的位移载体与命中判定体 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<USphereComponent> CollisionSphere;

    /** 表现网格（内容侧在蓝图子类里指定模型；不参与碰撞） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UStaticMeshComponent> ProjectileMesh;

    /** 投射物运动组件（初速 / 重力 / 旋转跟随速度） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
    TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

private:
    // -------------------------------------------------------------------------
    // 服务器权威结算数据（不复制：客户端不需要结算；这些字段只被服务器读写）
    // -------------------------------------------------------------------------
    /** 开火者（伤害归因来源；命中时用于取 ASC 与敌我判定） */
    UPROPERTY()
    TObjectPtr<AActor> DamageSource;

    /** 伤害效果类 */
    UPROPERTY()
    TSubclassOf<UGameplayEffect> DamageEffectClass;

    /** 命中伤害（原始值） */
    float DamageAmount = 0.0f;

    /** 是否已结算过命中：防止同一次命中被重复处理（扫掠可能对同一目标产生多次事件） */
    bool bSettlementDone = false;
};
