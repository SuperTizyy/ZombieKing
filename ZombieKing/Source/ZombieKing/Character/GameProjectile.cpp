#include "GameProjectile.h"

#include "GameAbilitySystemComponent.h"      // 伤害施加 + 敌我判定（IsHostileTarget 静态规则）
#include "AbilitySystemInterface.h"          // IAbilitySystemInterface（从开火者取 ASC）
#include "Components/SphereComponent.h"      // USphereComponent（根碰撞体）
#include "Components/StaticMeshComponent.h"  // UStaticMeshComponent（表现网格）
#include "GameFramework/ProjectileMovementComponent.h"  // UProjectileMovementComponent
#include "GameplayEffect.h"                  // UGameplayEffect（伤害效果类的完整类型）
#include "Engine/CollisionProfile.h"         // UCollisionProfile::BlockAllDynamic_ProfileName
#include "Engine/World.h"                    // UWorld
#include "ZombieKing.h"                      // LogZombieKing

AGameProjectile::AGameProjectile()
{
    // -------------------------------------------------------------------------
    // 复制：投射物是"世界内实体"，必须让所有客户端看到它的位置与速度
    // -------------------------------------------------------------------------
    bReplicates = true;
    // 复制根组件的位移与速度（ProjectileMovement 会把速度写入根组件，客户端据此得到同样的视觉轨迹）
    SetReplicatingMovement(true);

    // 投射物无需 Tick：位移与命中完全由 UProjectileMovementComponent 的扫掠驱动
    PrimaryActorTick.bCanEverTick = false;

    // -------------------------------------------------------------------------
    // 碰撞球（根组件）
    // -------------------------------------------------------------------------
    CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
    CollisionSphere->InitSphereRadius(8.0f);
    CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
    // BlockAllDynamic：与所有动态/静态物体发生阻挡（墙体、角色、可破坏物都能挡住投射物）
    CollisionSphere->SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
    // 角色不应把投射物当台阶踩上去
    CollisionSphere->CanCharacterStepUpOn = ECB_No;
    // 打开刚体碰撞通知：确保"被别的物体撞到"时也能收到命中事件（自身扫掠命中本身已能触发）
    CollisionSphere->SetNotifyRigidBodyCollision(true);
    SetRootComponent(CollisionSphere);

    // -------------------------------------------------------------------------
    // 表现网格（内容侧在蓝图子类里指定模型；不参与碰撞，否则会与碰撞球重复判定）
    // -------------------------------------------------------------------------
    ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
    ProjectileMesh->SetupAttachment(RootComponent);
    ProjectileMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    ProjectileMesh->bCastDynamicShadow = false;

    // -------------------------------------------------------------------------
    // 投射物运动
    // -------------------------------------------------------------------------
    ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
    // 更新载体不在此显式指定：UMovementComponent 在 InitializeComponent 时若发现 UpdatedComponent 为空，
    // 会自动绑定 Owner 的根组件（本项目根组件即碰撞球），这也是引擎模板投射物的做法；
    // 显式在构造期调用 SetUpdatedComponent 会触动物理体积查询，属于构造期不必承担的开销。
    // 具体初速/重力由 InitializeProjectile 按武器定义灌入，这里只给保守的类默认值
    ProjectileMovement->InitialSpeed = 3000.0f;
    ProjectileMovement->MaxSpeed = 3000.0f;
    ProjectileMovement->bRotationFollowsVelocity = true;
}

void AGameProjectile::BeginPlay()
{
    Super::BeginPlay();

    // 忽略开火者自身：否则投射物一出生就与开火者的胶囊重叠，立刻触发命中（把自己炸了）
    if (AActor* InstigatorActor = GetInstigator())
    {
        CollisionSphere->IgnoreActorWhenMoving(InstigatorActor, /*bShouldIgnore=*/true);
    }

    // 绑定命中事件（放在 BeginPlay 而非构造期：动态委托绑定要求对象已完成初始化）
    CollisionSphere->OnComponentHit.AddDynamic(this, &AGameProjectile::HandleComponentHit);
}

void AGameProjectile::InitializeProjectile(AActor* InDamageSource, float InDamage, TSubclassOf<UGameplayEffect> InDamageEffect,
                                           float InInitialSpeed, float InGravityScale, float InLifeSeconds)
{
    // 只有服务器持有权威结算数据；客户端不应也无法初始化（其投射物由复制产生）
    if (!HasAuthority())
    {
        return;
    }

    DamageSource = InDamageSource;
    DamageAmount = InDamage;
    DamageEffectClass = InDamageEffect;

    if (ProjectileMovement)
    {
        // 初速与上限：两者一致，避免"飞行途中被 MaxSpeed 慢慢加速"的怪异表现
        ProjectileMovement->InitialSpeed = InInitialSpeed;
        ProjectileMovement->MaxSpeed = InInitialSpeed;
        ProjectileMovement->ProjectileGravityScale = FMath::Max(InGravityScale, 0.0f);

        // 显式写入速度（世界空间、沿生成时的朝向）。
        // 不依赖"组件按 InitialSpeed 自行推导"：投射物在生成后才被初始化，组件的初始化时机已过，
        // 显式赋值是唯一可靠的路径（同时保证复制出去的初速度与服务器完全一致）。
        ProjectileMovement->Velocity = GetActorForwardVector() * InInitialSpeed;
    }

    // 存活上限：超时未命中即销毁（防止漏网投射物常驻场景；服务器销毁会复制到所有客户端）
    SetLifeSpan(FMath::Max(InLifeSeconds, 1.0f));
}

void AGameProjectile::HandleComponentHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
                                         UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit)
{
    // -------------------------------------------------------------------------
    // 客户端不结算伤害：命中表现由 GameplayCue 通道（伤害 GE 携带的 Cue 标签）在各端播放
    // -------------------------------------------------------------------------
    if (!HasAuthority())
    {
        return;
    }

    // 同一次命中只结算一次：扫掠可能对同一目标产生多次事件（尤其在薄墙/多组件目标上）
    if (bSettlementDone)
    {
        return;
    }
    bSettlementDone = true;

    // 命中即停碰撞：避免"已结算后继续穿行并再次触发"造成的重复命中
    if (CollisionSphere)
    {
        CollisionSphere->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    }

    // -------------------------------------------------------------------------
    // 伤害结算：只对"敌对的、带 ASC 的目标"结算；命中场景物件/友军只留弹着表现（由 Cue 层处理）
    // -------------------------------------------------------------------------
    if (OtherActor && OtherActor != DamageSource && DamageAmount > 0.0f && DamageEffectClass)
    {
        const IAbilitySystemInterface* SourceInterface = Cast<IAbilitySystemInterface>(DamageSource);
        UGameAbilitySystemComponent* SourceASC = SourceInterface
            ? Cast<UGameAbilitySystemComponent>(SourceInterface->GetAbilitySystemComponent())
            : nullptr;

        // 敌我判定走公共规则（与技能侧、武器组件同一套实现，避免规则漂移）
        if (SourceASC && UGameAbilitySystemComponent::IsHostileTarget(DamageSource, OtherActor))
        {
            // 带命中细节施加：受击方（UCombatComponent）据此做部位判定与受击硬直
            SourceASC->ApplyDamageToTargetWithHit(OtherActor, DamageAmount, DamageEffectClass, DamageSource, Hit);
        }
    }

    // 命中即销毁（投射物本体不需要"残留"，命中特效由 Cue 层负责）
    Destroy();
}
