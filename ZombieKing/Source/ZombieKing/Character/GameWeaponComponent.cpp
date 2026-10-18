#include "GameWeaponComponent.h"

#include "GameWeaponDefinition.h"            // 武器定义（全部数值来源）
#include "GameProjectile.h"                  // 投射物 Actor（投射物类武器）
#include "GameAbilitySystemComponent.h"      // 伤害施加 + 敌我判定（IsHostileTarget 静态规则）
#include "GameAbilityTags.h"                 // GameAbilityTags::Input_Fire（开火技能的输入标签）
#include "AbilitySystemInterface.h"          // IAbilitySystemInterface（从 Owner 取 ASC）
#include "Engine/World.h"                    // UWorld（射线检测 / 定时器）
#include "TimerManager.h"                    // FTimerManager（换弹计时）
#include "CollisionQueryParams.h"            // FCollisionQueryParams / SCENE_QUERY_STAT
#include "Engine/EngineTypes.h"              // ECC_Pawn / ESpawnActorCollisionHandlingMethod
#include "DrawDebugHelpers.h"                // DrawDebugLine（仅开发期）
#include "GameFramework/Pawn.h"              // APawn::IsLocallyControlled
#include "GameFramework/Actor.h"             // AActor::HasAuthority（组件需经 Owner 判断权威端）
#include "Net/UnrealNetwork.h"               // DOREPLIFETIME
#include "ZombieKing.h"                      // LogZombieKing

UGameWeaponComponent::UGameWeaponComponent()
{
    // 组件本身必须复制：槽位/弹药/换弹状态是客户端表现所必需的数据
    SetIsReplicatedByDefault(true);

    // 本组件不需要逐帧 Tick：一切由"开火/换弹/切枪"事件与定时器驱动
    // （大厂纪律：能事件驱动就不要 Tick，避免每帧无谓开销与顺序依赖）
    PrimaryComponentTick.bCanEverTick = false;
}

// =============================================================================
//  生命周期
// =============================================================================

void UGameWeaponComponent::BeginPlay()
{
    Super::BeginPlay();

    // 按装备槽初始化弹药（两端都做）：
    //  - 服务器：这是权威初始值，初始化后立即复制给客户端（"出生满弹匣"）；
    //  - 客户端：只为"复制到达之前"的极短窗口提供占位值（HUD 不会先显示 0），
    //            随后的服务器值会整体覆盖本地值，因此不会产生持久不一致。
    EnsureAmmoArraysInitialized();

    if (HasWeaponAuthority())
    {
        // 出生时选中首个槽位（通常配成主武器）；变异重生会生成新 Pawn → 重新走这条路径。
        SelectInitialSlotServer();
    }
}

void UGameWeaponComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // 清理换弹与连发定时器：本组件销毁后回调不得再进来（否则会打到已失效对象上）
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(ReloadTimerHandle);
        World->GetTimerManager().ClearTimer(AutoFireTimerHandle);
    }

    Super::EndPlay(EndPlayReason);
}

void UGameWeaponComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);

    // 复制清单（刻意保持最小：只复制"客户端表现与 HUD 需要"的状态）
    DOREPLIFETIME(UGameWeaponComponent, CurrentSlotIndex);
    DOREPLIFETIME(UGameWeaponComponent, SlotAmmo);
    DOREPLIFETIME(UGameWeaponComponent, SlotReserveAmmo);
    DOREPLIFETIME(UGameWeaponComponent, bReloading);
}

// =============================================================================
//  复制回调（客户端）
// =============================================================================

void UGameWeaponComponent::OnRep_CurrentSlotIndex()
{
    // 换枪：模型/动画/准星状态由 Lua 在 OnWeaponChanged 里刷新；弹药显示也随之切换（每把枪各自的弹药）
    NotifyWeaponChanged();
    NotifyAmmoChanged();
}

void UGameWeaponComponent::OnRep_AmmoState()
{
    NotifyAmmoChanged();
}

void UGameWeaponComponent::OnRep_ReloadState()
{
    // 进入换弹：在本端记录起始时刻，供 GetReloadProgress 计算进度（不依赖跨机时钟同步）
    if (bReloading)
    {
        MarkReloadStartLocalTime();
    }

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    NotifyReloadStateChanged(bReloading, Weapon ? Weapon->ReloadSeconds : 0.0f);
}

void UGameWeaponComponent::Multicast_Fired_Implementation()
{
    // 本地控制的角色（"我自己"，以及 ListenServer 主机本人）跳过本广播：
    // 其开火表现已由输入回调**即时本地播放**（见 GameCharacterBase.lua），
    // 若此处再播一次会出现双重音效与枪口火焰重影。
    // 换言之：本多播只服务"别人在我屏幕上看到的开火"。
    const APawn* OwnerPawn = Cast<APawn>(GetOwner());
    if (OwnerPawn && OwnerPawn->IsLocallyControlled())
    {
        return;
    }

    NotifyFired();
}

void UGameWeaponComponent::ClientConfirmAmmo_Implementation(int32 SlotIndex, int32 ConfirmedAmmo, int32 ConfirmedReserveAmmo)
{
    // 槽位/数组非法（武器配置变动、槽尚未初始化等）：丢弃本次确认，等下一次开火/复制对齐
    if (!SlotAmmo.IsValidIndex(SlotIndex) || !SlotReserveAmmo.IsValidIndex(SlotIndex))
    {
        return;
    }

    // 值完全一致（正常路径：本地预测准确，服务器复制也即将带来同样的值）：无需任何动作，
    // 避免多余的 HUD 刷新广播（ListenServer 本端执行到这里时必然走本分支）。
    if (SlotAmmo[SlotIndex] == ConfirmedAmmo && SlotReserveAmmo[SlotIndex] == ConfirmedReserveAmmo)
    {
        return;
    }

    // 对账：本地预测值与权威值有偏差（如预测请求被服务器拒绝、或多包乱序的罕见时序）——
    // 直接采用服务器权威值。这是一次性的小跳变（±1 发量级），换来的是偏差绝不残留。
    SlotAmmo[SlotIndex] = ConfirmedAmmo;
    SlotReserveAmmo[SlotIndex] = ConfirmedReserveAmmo;
    NotifyAmmoChanged();
}

// =============================================================================
//  权威动作：开火（本地请求 → GAS 技能 → 服务器节流 → 组件结算）
// =============================================================================

void UGameWeaponComponent::StartFire()
{
    // 立即请求一次（单发武器就这一次；全自动武器随后由本地定时器按射速节奏反复请求）
    RequestFireLocally();

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    if (!Weapon || Weapon->FireMode != EGameWeaponFireMode::Auto)
    {
        // 无武器 / 单发武器：不起连发定时器
        return;
    }

    // 已在连发中则直接返回（幂等：StartFire 可能被多次调用，如"按下"事件重复到达）
    if (AutoFireTimerHandle.IsValid())
    {
        return;
    }

    const float Interval = GetAutoFireRequestInterval();
    if (Interval <= 0.0f)
    {
        // RoundsPerMinute <= 0（不限射速的调试/特殊武器）：不做连发定时器，避免 0 间隔定时器风暴
        return;
    }

    if (UWorld* World = GetWorld())
    {
        // 循环定时器：按射速节奏请求开火；射速/是否连发都来自武器定义，改资产即生效
        World->GetTimerManager().SetTimer(AutoFireTimerHandle, this, &UGameWeaponComponent::RepeatFireRequestTimer,
            Interval, /*bLoop=*/true);
    }
}

void UGameWeaponComponent::StopFire()
{
    // 幂等停表：未在连发时 ClearTimer 是安全空操作
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(AutoFireTimerHandle);
    }
}

void UGameWeaponComponent::RepeatFireRequestTimer()
{
    // 连发定时器回调：每次只是"再请求一次开火"，实际能否开火由服务器节流裁决
    // 注意：换弹期间 RequestFireLocally 会自行跳过请求但**不停表**，
    //       这样玩家一直按住开火键时，换弹完成会自动继续开火（符合射击游戏手感）。
    RequestFireLocally();
}

void UGameWeaponComponent::RequestFireLocally()
{
    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    if (!Weapon)
    {
        return;
    }

    // 换弹中：本地不请求（服务器也必然拒绝），但保持连发定时器运行以便换弹完成后自动续火
    if (bReloading)
    {
        return;
    }

    // -------------------------------------------------------------------------
    // 客户端节拍预检（只有纯客户端做；服务器/主机由 TryFire 的锁相闸门自行裁决）
    // -------------------------------------------------------------------------
    // 与服务器锁相闸门同节拍（标称射速间隔）：超频请求在这里静默丢弃 ——
    // 它们注定被服务器拒绝，而本地预测扣弹又收不回，会造成"HUD 弹药比真实少"的幽灵弹匣。
    // 客户端本地没有网络抖动（请求发出的时刻即本端时间），因此无需容差，直接按标称间隔卡。
    const bool bServerSide = HasWeaponAuthority();
    UWorld* World = GetWorld();
    const float Now = World ? World->GetTimeSeconds() : 0.0f;
    const float FireInterval = Weapon->GetFireIntervalSeconds();
    if (!bServerSide)
    {
        if (FireInterval > 0.0f && Now < LastLocalFireRequestTime + FireInterval)
        {
            return;
        }

        // 本地预判空仓：只播"空仓"表现，不发开火请求（省一次无谓的技能激活/网络请求）。
        // 预测扣弹会把本地弹匣扣到 0，此后本分支自然接管空仓表现，无需额外状态。
        if (GetAmmo() <= 0)
        {
            OnEmptyTriggered.Broadcast();
            return;
        }

        // 客户端本地预测扣弹：HUD 立即 -1，不等服务器复制（消除一个 RTT 的显示滞后）。
        // 权威值随后由 ClientConfirmAmmo（每次成功开火）或属性复制（换弹/切枪）对齐。
        PredictAmmoLocally(GetResolvedSlotIndex());

        LastLocalFireRequestTime = Now;
    }
    else if (GetAmmo() <= 0)
    {
        // 服务器/主机本端的空仓预判（行为与客户端一致：只播空仓表现，不发请求）
        OnEmptyTriggered.Broadcast();
        return;
    }

    // 请求开火：走 GAS（技能是权威入口闸门，被定身/缴械等阻断可由技能标签体系统一生效）
    if (UGameAbilitySystemComponent* AbilitySystemComponent = GetOwnerAbilitySystemComponent())
    {
        AbilitySystemComponent->TryActivateAbilitiesByInputTag(GameAbilityTags::Input_Fire);
    }

    // 本端即时表现（本地预测）：不等服务器回包就播枪口火焰/后坐力/音效 —— 这是竞技射击的必需手感。
    // 服务器实际开火时，其他客户端由 Multicast_Fired 广播；开火者本人被该多播刻意跳过（避免重影/双声）。
    NotifyFired();
}

void UGameWeaponComponent::PredictAmmoLocally(int32 Slot)
{
    // 只在当前槽有弹药可扣时预测：GetAmmo() <= 0 的分支已在调用前拦截，这里是双保险
    if (!SlotAmmo.IsValidIndex(Slot) || SlotAmmo[Slot] <= 0)
    {
        return;
    }

    // 直接修改复制属性的本地副本（客户端上允许，随后会被服务器复制/确认覆盖）
    SlotAmmo[Slot] -= 1;
    NotifyAmmoChanged();
}

float UGameWeaponComponent::GetAutoFireRequestInterval() const
{
    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    if (!Weapon)
    {
        return 0.0f;
    }

    // 请求节奏 = 武器标称射速间隔本身。
    // 刻意不"提前请求"（例如乘 0.9）：提前请求会让"本地预测表现"多于服务器实际开火（视觉与判定不一致）；
    // 让请求恰好按标称节奏发出，靠服务器侧的到达抖动容差吸收偏差 —— 这样观感射速与配置值一致。
    return Weapon->GetFireIntervalSeconds();
}

bool UGameWeaponComponent::TryFire(FVector AimLocation, FRotator AimRotation)
{
    // -------------------------------------------------------------------------
    // 双保险：客户端不得裁定开火与伤害
    // -------------------------------------------------------------------------
    if (!HasWeaponAuthority())
    {
        return false;
    }

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    if (!Weapon)
    {
        // 没有武器（未配装 / 当前角色不该有枪）：直接拒绝。属配置问题，不逐次告警以免刷屏。
        return false;
    }

    // 换弹中不能开火（竞技射击惯例：换弹是"硬承诺"，需靠切枪打断或等待完成）
    if (bReloading)
    {
        return false;
    }

    const int32 Slot = GetResolvedSlotIndex();
    if (!SlotAmmo.IsValidIndex(Slot))
    {
        return false;
    }

    UWorld* World = GetWorld();
    if (!World)
    {
        return false;
    }

    // -------------------------------------------------------------------------
    // ① 射速节流（服务器权威反作弊的核心）：节拍锁相闸门（只判不推）
    // -------------------------------------------------------------------------
    // 客户端可以任意高频地请求开火（含按住连发），但服务器只按武器射速放行；
    // 因此"提高点击频率"对 DPS 没有任何收益，客户端无法凭此作弊。
    //
    // 锁相方案（对比旧"间隔 - 容差"与"滑动窗口"的说明见 NextAllowedFireServerTime 注释）：
    //  - 允许时刻 = NextAllowedFireServerTime，由"放行后 += 间隔"的节拍累加推进；
    //  - 容差只用于吸收"请求到达"的定时器粒度与网络抖动（允许单发早到）；
    //  - 此处只做"判"，节拍推进放在 ③（通过全部闸门后）——
    //    空仓 / 换弹等"没打出去"的拒绝不消耗节拍，玩家下一发不必额外等待，更公平。
    const float Now = World->GetTimeSeconds();
    const float FireInterval = Weapon->GetFireIntervalSeconds();
    if (FireInterval > 0.0f && Now < NextAllowedFireServerTime - ServerFireThrottleToleranceSeconds)
    {
        // 处于上一发的节拍窗口内：静默拒绝（高频请求属正常现象，不做告警以免刷屏）
        return false;
    }

    // -------------------------------------------------------------------------
    // ② 空仓：不开火；按武器定义决定是否自动换弹
    // -------------------------------------------------------------------------
    if (SlotAmmo[Slot] <= 0)
    {
        if (Weapon->bAutoReloadWhenEmpty)
        {
            // 注意：RequestReload 内部同样做权威校验，这里无需重复判断
            RequestReload();
        }
        return false;
    }

    // -------------------------------------------------------------------------
    // ③ 通过全部闸门：推进节拍 + 扣弹
    // -------------------------------------------------------------------------
    // 节拍锁相推进：正常按节拍累加（作弊者贴边请求无法前移基准）；晚到/首火则重新同步到当前时刻
    // （max 的第二支：停火很久后再开火，节拍从 Now 重启，不产生"补发"）。
    if (FireInterval > 0.0f)
    {
        NextAllowedFireServerTime = FMath::Max(NextAllowedFireServerTime + FireInterval, Now + FireInterval);
    }

    SlotAmmo[Slot] -= 1;
    // 服务器修改复制属性后不会触发 RepNotify，故必须手动广播一次，
    // 保证同端（ListenServer 的本地 HUD / Lua）立即看到弹药变化。
    NotifyAmmoChanged();

    // 开火确认（定向不可靠 Client RPC）：把扣弹后的权威弹匣/备弹带给开火者，
    // 对齐其本地预测值（见 ClientConfirmAmmo 注释）。ListenServer 本端会立即本地执行，幂等无副作用。
    ClientConfirmAmmo(Slot, SlotAmmo[Slot], SlotReserveAmmo.IsValidIndex(Slot) ? SlotReserveAmmo[Slot] : 0);

    // -------------------------------------------------------------------------
    // ④ 弹道结算：即时射线（可多发=霰弹）或投射物
    // -------------------------------------------------------------------------
    if (Weapon->PayloadKind == EGameWeaponPayloadKind::Hitscan)
    {
        PerformHitscanServer(*Weapon, AimLocation, AimRotation);
    }
    else
    {
        SpawnProjectileServer(*Weapon, AimLocation, AimRotation);
    }

    // -------------------------------------------------------------------------
    // ⑤ 开火表现广播（不可靠多播；本端由输入路径自行播放，见 Multicast_Fired_Implementation）
    // -------------------------------------------------------------------------
    Multicast_Fired();
    return true;
}

void UGameWeaponComponent::PerformHitscanServer(const UGameWeaponDefinition& Weapon, const FVector& AimLocation,
                                               const FRotator& AimRotation)
{
    UWorld* World = GetWorld();
    AActor* OwnerActor = GetOwner();
    if (!World || !OwnerActor)
    {
        return;
    }

    // 施加方 ASC：走 IAbilitySystemInterface（本项目 ASC 托管在 PlayerState，见 AGamePlayerState）
    UGameAbilitySystemComponent* SourceASC = GetOwnerAbilitySystemComponent();

    // -------------------------------------------------------------------------
    // 弹丸循环：PelletCount > 1 即霰弹 —— 一次开火发射多颗，每颗独立射线、独立结算
    // -------------------------------------------------------------------------
    const int32 PelletCount = FMath::Max(Weapon.PelletCount, 1);
    const float HalfConeRadians = FMath::DegreesToRadians(Weapon.SpreadAngleDegrees * 0.5f);
    const FVector BaseDirection = AimRotation.Vector();

    // 射线追踪参数：忽略开火者自身（否则会立刻命中自己的身体胶囊导致"打不到人"）
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(GameWeaponHitscan), /*bTraceComplex=*/false, OwnerActor);

    for (int32 PelletIndex = 0; PelletIndex < PelletCount; ++PelletIndex)
    {
        // 散布：零散布为精准直射；有散布时在锥体内随机偏转（霰弹扩散 / 腰射手感）
        const FVector ShotDirection = (Weapon.SpreadAngleDegrees > 0.0f)
            ? FMath::VRandCone(BaseDirection, HalfConeRadians)
            : BaseDirection;

        const FVector TraceEnd = AimLocation + ShotDirection * Weapon.TraceRange;

        FHitResult HitResult;
        const bool bHit = World->LineTraceSingleByChannel(HitResult, AimLocation, TraceEnd, Weapon.TraceChannel, QueryParams);

#if ENABLE_DRAW_DEBUG
        if (bDrawDebugTrace)
        {
            // 开发期可视化：命中为红、未命中为绿，便于校准准星与真实弹道是否一致
            const FVector DrawEnd = bHit ? HitResult.ImpactPoint : TraceEnd;
            DrawDebugLine(World, AimLocation, DrawEnd, bHit ? FColor::Red : FColor::Green, false, 1.0f, 0, 1.0f);
        }
#endif

        // 未命中 / 命中场景物件 / 无 ASC 可结算 → 只留弹着表现（由客户端与 Cue 层处理），不结算伤害
        if (!bHit || !HitResult.GetActor() || !SourceASC)
        {
            continue;
        }

        // 敌我判定走公共规则（与技能侧同一套实现，避免两处规则漂移）：
        // 友军与场景物件不结算伤害，但"打中了什么"这个物理事实已经发生（弹着表现照旧）。
        if (!UGameAbilitySystemComponent::IsHostileTarget(OwnerActor, HitResult.GetActor()))
        {
            continue;
        }

        // 伤害结算：把命中细节写入效果上下文，受击方（UCombatComponent）据此做部位判定与硬直
        SourceASC->ApplyDamageToTargetWithHit(HitResult.GetActor(), Weapon.DamagePerPellet,
            Weapon.DamageEffectClass, OwnerActor, HitResult);
    }
}

void UGameWeaponComponent::SpawnProjectileServer(const UGameWeaponDefinition& Weapon, const FVector& AimLocation,
                                                const FRotator& AimRotation)
{
    UWorld* World = GetWorld();
    AActor* OwnerActor = GetOwner();
    if (!World || !OwnerActor)
    {
        return;
    }

    if (!Weapon.ProjectileClass)
    {
        // 配了投射物类武器却没填投射物类：属配置缺失，显式告警（否则"开枪没反应"极难定位）
        UE_LOG(LogZombieKing, Warning, TEXT("[武器组件] 武器 %s 为投射物类型但未配置 ProjectileClass，本次开火无效果"),
            *Weapon.GetName());
        return;
    }

    // 散布对投射物同样适用（榴弹扩散等）；方向先加散布再转旋转
    FVector FireDirection = AimRotation.Vector();
    if (Weapon.SpreadAngleDegrees > 0.0f)
    {
        FireDirection = FMath::VRandCone(FireDirection, FMath::DegreesToRadians(Weapon.SpreadAngleDegrees * 0.5f));
    }

    // -------------------------------------------------------------------------
    // 生成投射物（服务器权威：只有服务器生成并初始化，客户端由复制获得表现）
    // 生成点取"瞄准起点"（服务器上即客户端上报的相机 POV）：服务器没有第一人称枪模型，
    // 而投射物本身会与墙体做扫掠碰撞，因此从相机点出生不会产生穿墙问题。
    // -------------------------------------------------------------------------
    FActorSpawnParameters SpawnParams;
    SpawnParams.Owner = OwnerActor;                          // 归属开火者（生命周期与归因）
    SpawnParams.Instigator = Cast<APawn>(OwnerActor);        // 用于投射物忽略自身碰撞
    SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

    AGameProjectile* Projectile = World->SpawnActor<AGameProjectile>(
        Weapon.ProjectileClass, AimLocation, FireDirection.Rotation(), SpawnParams);
    if (!Projectile)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[武器组件] 投射物生成失败（类 %s）"), *Weapon.ProjectileClass->GetName());
        return;
    }

    // 发射参数由武器定义灌入：伤害/效果类/初速/重力/存活时长
    Projectile->InitializeProjectile(OwnerActor, Weapon.DamagePerPellet, Weapon.DamageEffectClass,
        Weapon.ProjectileInitialSpeed, Weapon.ProjectileGravityScale, Weapon.ProjectileLifeSeconds);
}

// =============================================================================
//  权威动作：换弹
// =============================================================================

bool UGameWeaponComponent::RequestReload()
{
    if (!HasWeaponAuthority())
    {
        return false;
    }

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    if (!Weapon || bReloading)
    {
        return false;
    }

    const int32 Slot = GetResolvedSlotIndex();
    if (!SlotAmmo.IsValidIndex(Slot) || !SlotReserveAmmo.IsValidIndex(Slot))
    {
        return false;
    }

    // 弹匣已满 / 无备弹：无可装填，拒绝（玩家按 R 不会有任何反馈，属预期行为）
    if (SlotAmmo[Slot] >= Weapon->MagazineSize || SlotReserveAmmo[Slot] <= 0)
    {
        return false;
    }

    // 配置为 0 秒 = 立即完成（调试或特殊武器）：不走定时器，直接搬运弹药
    if (Weapon->ReloadSeconds <= 0.0f)
    {
        FinishReloadServer();
        return true;
    }

    // -------------------------------------------------------------------------
    // 服务器权威计时：换弹完成由服务器裁决（客户端不做本地预测）
    // 理由：换弹是"资源恢复"动作，若客户端提前放行，会出现"客户端已满弹匣、服务器仍空仓"的
    //       不一致窗口（后续开火被服务器拒绝，玩家会觉得"卡了"）。宁可让客户端表现等服务器。
    // -------------------------------------------------------------------------
    bReloading = true;
    MarkReloadStartLocalTime();
    NotifyReloadStateChanged(true, Weapon->ReloadSeconds);

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().SetTimer(ReloadTimerHandle, this, &UGameWeaponComponent::FinishReloadServer,
            Weapon->ReloadSeconds, /*bLoop=*/false);
    }
    return true;
}

void UGameWeaponComponent::FinishReloadServer()
{
    if (!HasWeaponAuthority())
    {
        return;
    }

    // 先清定时器句柄（0 秒换弹路径没有定时器，ClearTimer 对空句柄同样安全）
    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(ReloadTimerHandle);
    }

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    const int32 Slot = GetResolvedSlotIndex();
    float ReloadSeconds = 0.0f;

    if (Weapon && SlotAmmo.IsValidIndex(Slot) && SlotReserveAmmo.IsValidIndex(Slot))
    {
        ReloadSeconds = Weapon->ReloadSeconds;

        // 从备弹补满弹匣：补入量 = min(弹匣缺口, 备弹余量)
        // 刻意保留"换弹前已装填的子弹"（不做"弹匣清零再装填"）—— 后者虽更拟真，
        // 但在快节奏竞技里会让玩家因误触换弹而白白损失半弹匣，属于手感负收益。
        const int32 Needed = FMath::Max(Weapon->MagazineSize - SlotAmmo[Slot], 0);
        const int32 Loaded = FMath::Min(Needed, FMath::Max(SlotReserveAmmo[Slot], 0));
        SlotAmmo[Slot] += Loaded;
        SlotReserveAmmo[Slot] -= Loaded;
    }

    bReloading = false;

    // 服务器改值不触发 RepNotify：手动广播（同端的 HUD / Lua 才能即时更新）
    NotifyReloadStateChanged(false, ReloadSeconds);
    NotifyAmmoChanged();
}

void UGameWeaponComponent::CancelReloadServer()
{
    if (!HasWeaponAuthority())
    {
        return;
    }

    if (UWorld* World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(ReloadTimerHandle);
    }

    // 未在换弹则无需任何广播（避免切枪时产生多余的状态事件）
    if (!bReloading)
    {
        return;
    }

    bReloading = false;
    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    NotifyReloadStateChanged(false, Weapon ? Weapon->ReloadSeconds : 0.0f);
}

// =============================================================================
//  权威动作：切枪
// =============================================================================

bool UGameWeaponComponent::RequestSwitchWeapon(int32 SlotIndex)
{
    if (!HasWeaponAuthority())
    {
        return false;
    }

    if (!WeaponSlots.IsValidIndex(SlotIndex))
    {
        return false;
    }

    // 已在该槽位：幂等返回 false（不产生多余广播）
    if (SlotIndex == GetResolvedSlotIndex())
    {
        return false;
    }

    // 切枪打断换弹（竞技射击惯例）：既符合手感，也堵住"换弹到一半切走又切回来弹匣已满"的状态漏洞
    CancelReloadServer();

    CurrentSlotIndex = SlotIndex;

    // 服务器改值不触发 RepNotify：手动广播"换枪 + 弹药"（每把枪弹药不同，两者都要通知）
    NotifyWeaponChanged();
    NotifyAmmoChanged();
    return true;
}

bool UGameWeaponComponent::RequestNextWeapon()
{
    if (!HasWeaponAuthority())
    {
        return false;
    }

    const int32 SlotCount = WeaponSlots.Num();
    if (SlotCount <= 1)
    {
        // 只有一把枪（或没枪）时，"下一把"就是原地踏步：不做任何事
        return false;
    }

    const int32 CurrentSlot = FMath::Max(GetResolvedSlotIndex(), 0);
    const int32 NextSlot = (CurrentSlot + 1) % SlotCount;
    return RequestSwitchWeapon(NextSlot);
}

// =============================================================================
//  查询
// =============================================================================

UGameWeaponDefinition* UGameWeaponComponent::GetCurrentWeapon() const
{
    const int32 Slot = GetResolvedSlotIndex();
    return WeaponSlots.IsValidIndex(Slot) ? WeaponSlots[Slot] : nullptr;
}

int32 UGameWeaponComponent::GetCurrentSlotIndex() const
{
    return GetResolvedSlotIndex();
}

UGameWeaponDefinition* UGameWeaponComponent::GetWeaponAtSlot(int32 SlotIndex) const
{
    return WeaponSlots.IsValidIndex(SlotIndex) ? WeaponSlots[SlotIndex] : nullptr;
}

int32 UGameWeaponComponent::GetAmmo() const
{
    const int32 Slot = GetResolvedSlotIndex();
    return SlotAmmo.IsValidIndex(Slot) ? SlotAmmo[Slot] : 0;
}

int32 UGameWeaponComponent::GetReserveAmmo() const
{
    const int32 Slot = GetResolvedSlotIndex();
    return SlotReserveAmmo.IsValidIndex(Slot) ? SlotReserveAmmo[Slot] : 0;
}

int32 UGameWeaponComponent::GetMagazineSize() const
{
    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    return Weapon ? Weapon->MagazineSize : 0;
}

float UGameWeaponComponent::GetReloadProgress() const
{
    if (!bReloading)
    {
        return 0.0f;
    }

    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    const float TotalSeconds = Weapon ? Weapon->ReloadSeconds : 0.0f;
    if (TotalSeconds <= 0.0f || !GetWorld())
    {
        // 0 秒换弹（立即完成）或世界不可用：直接视为已完成，避免出现除零
        return 1.0f;
    }

    const float ElapsedSeconds = GetWorld()->GetTimeSeconds() - LocalReloadStartTime;
    return FMath::Clamp(ElapsedSeconds / TotalSeconds, 0.0f, 1.0f);
}

bool UGameWeaponComponent::IsCurrentWeaponAutomatic() const
{
    const UGameWeaponDefinition* Weapon = GetCurrentWeapon();
    return Weapon && Weapon->FireMode == EGameWeaponFireMode::Auto;
}

// =============================================================================
//  内部实现
// =============================================================================

bool UGameWeaponComponent::HasWeaponAuthority() const
{
    // 组件本身没有 HasAuthority()（那是 AActor 的方法），必须经 Owner 判断；
    // 同时防 Owner 为空（组件已从 Actor 上摘除的极端情况）
    const AActor* OwnerActor = GetOwner();
    return OwnerActor && OwnerActor->HasAuthority();
}

int32 UGameWeaponComponent::GetResolvedSlotIndex() const
{
    // 当前槽位合法则用它；否则回退到 0。
    // 为什么需要回退：客户端在"复制到达之前"CurrentSlotIndex 仍是类默认值 INDEX_NONE，
    // 若不回退，HUD 与表现层会在开局头几帧拿到空武器（显示为空、模型不出现）。
    if (WeaponSlots.IsValidIndex(CurrentSlotIndex))
    {
        return CurrentSlotIndex;
    }
    return WeaponSlots.Num() > 0 ? 0 : INDEX_NONE;
}

void UGameWeaponComponent::EnsureAmmoArraysInitialized()
{
    const int32 SlotCount = WeaponSlots.Num();

    if (SlotCount == 0)
    {
        SlotAmmo.Reset();
        SlotReserveAmmo.Reset();
        return;
    }

    // 已按当前槽位数初始化过则跳过（避免重复初始化把服务器已消耗的弹药又填满）
    if (SlotAmmo.Num() == SlotCount && SlotReserveAmmo.Num() == SlotCount)
    {
        return;
    }

    SlotAmmo.SetNum(SlotCount);
    SlotReserveAmmo.SetNum(SlotCount);

    for (int32 Index = 0; Index < SlotCount; ++Index)
    {
        const UGameWeaponDefinition* Weapon = WeaponSlots[Index];
        // 空槽位视为"无武器"：弹药为 0（查询接口也会因空定义而返回 0）
        SlotAmmo[Index] = Weapon ? Weapon->MagazineSize : 0;
        SlotReserveAmmo[Index] = Weapon ? Weapon->StartingReserveAmmo : 0;
    }
}

void UGameWeaponComponent::SelectInitialSlotServer()
{
    // 没有可用槽位（角色没配枪）：不设置槽位，保持 INDEX_NONE，后续开火会被安全拒绝
    if (!WeaponSlots.IsValidIndex(0))
    {
        return;
    }

    CurrentSlotIndex = 0;

    // 服务器改值不触发 RepNotify：手动广播初始状态
    NotifyWeaponChanged();
    NotifyAmmoChanged();
}

void UGameWeaponComponent::MarkReloadStartLocalTime()
{
    LocalReloadStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
}

UGameAbilitySystemComponent* UGameWeaponComponent::GetOwnerAbilitySystemComponent() const
{
    // 走 IAbilitySystemInterface 解耦具体角色类：本项目 ASC 托管在 PlayerState 上，
    // 但组件只关心"Owner 能不能提供 ASC"，不关心它托管在哪里（便于后续扩展 AI/载具等 Owner）。
    const IAbilitySystemInterface* AbilityInterface = Cast<IAbilitySystemInterface>(GetOwner());
    return AbilityInterface ? Cast<UGameAbilitySystemComponent>(AbilityInterface->GetAbilitySystemComponent()) : nullptr;
}

void UGameWeaponComponent::NotifyAmmoChanged()
{
    OnAmmoChanged.Broadcast(GetAmmo(), GetReserveAmmo());
}

void UGameWeaponComponent::NotifyWeaponChanged()
{
    OnWeaponChanged.Broadcast(GetResolvedSlotIndex(), GetCurrentWeapon());
}

void UGameWeaponComponent::NotifyReloadStateChanged(bool bInReloading, float InReloadSeconds)
{
    OnReloadStateChanged.Broadcast(bInReloading, InReloadSeconds);
}

void UGameWeaponComponent::NotifyFired()
{
    OnFired.Broadcast(GetCurrentWeapon());
}
