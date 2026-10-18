#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameWeaponTypes.h"   // EGameWeaponFireMode（供 Lua 判断"按住是否连发"）
#include "GameWeaponComponent.generated.h"

class UGameWeaponDefinition;   // 武器定义（数值全在该资产里）
class AGameProjectile;         // 投射物 Actor（投射物类武器使用）

/** 当前武器切换（SlotIndex = 新槽位；客户端据此换枪模型/动画） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameWeaponChanged, int32, SlotIndex, UGameWeaponDefinition*, Weapon);

/** 弹药变化（Ammo = 当前武器弹匣内子弹；ReserveAmmo = 当前武器备弹） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameWeaponAmmoChanged, int32, Ammo, int32, ReserveAmmo);

/** 换弹状态变化（bReloading = 是否正在换弹；ReloadSeconds = 本次换弹总时长，供进度条与动画播放速率换算） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameWeaponReloadStateChanged, bool, bReloading, float, ReloadSeconds);

/** 已开火（各端表现入口：后坐力/枪口火焰/音效/曳光弹；不携带命中结果 —— 命中反馈走 GameplayCue） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameWeaponFired, UGameWeaponDefinition*, Weapon);

/** 空仓扣扳机（本地即时表现入口："枪机空击"声、HUD 弹匣警示；不涉及任何权威结算） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnGameWeaponEmptyTriggered);

/**
 * 竞技生化网游：武器组件（挂在角色上，武器系统的**服务器权威**所在）
 *
 * 职责（"机制在 C++、数值在资产、表现在下沉脚本"三分离）：
 *  - **权威状态**：装备槽、每槽弹药（弹匣 + 备弹）、当前槽位、换弹状态 —— 全部是服务器权威并复制；
 *  - **权威动作**：开火（射速节流 + 扣弹 + 命中结算 / 生成投射物）、换弹、切枪；
 *  - **只读查询 + 事件**：把状态以 BlueprintPure / BlueprintAssignable 暴露给 HUD 与 Lua 表现层。
 *
 * 为什么武器不做成网络 Actor（本项目已拍板：武器定义资产 + 武器组件）：
 *  - 第一人称竞技射击里，玩家看到的枪只是**本地表现**（挂在自己 1P 手臂骨架上），
 *    把它做成联机 Actor 会平白增加复制带宽，并与"感染变异时销毁旧 Pawn"的生命周期纠缠；
 *  - 因此"武器的身份与状态"由本组件承担，**枪的模型/动画/音效由内容与 Lua 挂在角色挂点上表现**。
 *
 * 与 GAS 的关系（不重复、不越界）：
 *  - 开火入口仍是技能（UGameRangedAttackAbility）：输入标签 → 技能激活 → 调用本组件的 TryFire；
 *    这样"被定身/眩晕时不能开枪"等阻断可复用 GAS 的技能阻断能力；
 *  - 但"射速 / 弹药 / 换弹 / 切枪"这些**武器实例状态**只存在于本组件，
 *    避免散落到多个技能里导致状态机打架（这也是"每把枪各自弹药"能成立的原因）。
 *
 * 热更新边界：
 *  - 射速/弹匣/换弹时长/伤害/散布/投射物参数/表现参数全部来自 UGameWeaponDefinition（改资产即生效）；
 *  - "允许什么节奏开火"的节流裁决、弹药与换弹状态机属不可热更的机制，在 C++；
 *  - "开火表现（后坐力/准星/音效/动画）"与"开火的输入节奏（按住连发/点射）"由 Lua 决定。
 *
 * 内容侧使用：
 *  1) 在角色蓝图的武器组件 Details 里填 WeaponSlots（顺序即槽位顺序：0 通常是主武器）；
 *  2) 各槽填 UGameWeaponDefinition 资产；PlayerState 的阵营/角色定义切换会自动换 Avatar，
 *     新 Avatar 的组件在自己的 BeginPlay 重新按槽位初始化弹药（变异后弹药自然重置，符合竞技惯例）。
 */
UCLASS(ClassGroup = (ZombieKing), meta = (BlueprintSpawnableComponent))
class ZOMBIEKING_API UGameWeaponComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UGameWeaponComponent();

    // -------------------------------------------------------------------------
    // 装备槽配置（内容侧在角色蓝图里填；两端共享同一份类默认值，因此无需复制）
    // -------------------------------------------------------------------------
    /**
     * 武器槽位列表（顺序 = 槽位顺序）。
     * 说明：这是"角色身上带哪些枪"的装配，属内容配置；运行时状态（当前槽/弹药）在下方复制字段里。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Weapon|Loadout")
    TArray<TObjectPtr<UGameWeaponDefinition>> WeaponSlots;

    // -------------------------------------------------------------------------
    // 权威动作（BlueprintCallable：供 GAS 技能 / Lua 调用；内部一律校验服务器权威）
    // -------------------------------------------------------------------------
    /**
     * 开始开火（按下开火键时由 Lua 调用）。
     * 语义：
     *  - 立即请求开火一次；
     *  - 若当前武器是**全自动**（FireMode = Auto），额外在本端启动一个"按射速节奏反复请求开火"的
     *    本地定时器，直到 StopFire 被调用（松手 / 卸下）；
     *  - 单发武器只请求这一次。
     *
     * 为什么"连发节奏"由 C++ 组件持有，而不是 Lua 定时器：
     *  它需要毫秒级、与帧率无关的稳定重复触发，用引擎定时器最可靠、开销最低（Lua 侧做定时器还需跨语言回调）。
     *  但**节奏数值**来自武器定义（RoundsPerMinute，改资产即生效）、是否连发来自 FireMode（资产），
     *  因此依然满足"数值可热更"的硬要求；而"开火表现"仍全部在 Lua。
     */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Fire")
    void StartFire();

    /** 停止开火（松开开火键 / 卸下武器时由 Lua 调用；幂等） */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Fire")
    void StopFire();

    /**
     * 尝试开火一次（**服务器权威**）。
     * 内部顺序：① 权威校验 → ② 取当前武器 → ③ 换弹中则拒绝 → ④ 射速节流
     *          → ⑤ 空仓则拒绝（按需自动换弹）→ ⑥ 扣弹 → ⑦ 命中结算或生成投射物 → ⑧ 广播"已开火"。
     *
     * 射速节流放在服务器是刻意设计：客户端只"表达开火意愿"（含按住连发），
     * 无论客户端多快调用，实际开火速率都由服务器上限，客户端无法凭高频请求作弊（无散射加成、无额外伤害）。
     *
     * @param AimLocation 瞄准起点（服务器上即客户端上报的相机 POV，见 UGameGameplayAbility::GetAttackAimViewPoint）
     * @param AimRotation 瞄准方向
     * @return 本次是否真的开火（弹药不足/节流内/换弹中/无武器均返回 false）
     */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Fire")
    bool TryFire(FVector AimLocation, FRotator AimRotation);

    /**
     * 请求换弹（**服务器权威**）。
     * 拒绝条件：已在换弹 / 弹匣已满 / 无备弹 / 无武器 / 换弹时长为 0 以外的异常。
     * 换弹完成时从备弹补满弹匣；**切枪会打断换弹**（竞技射击惯例：切枪即放弃本次装填）。
     */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Reload")
    bool RequestReload();

    /** 请求切到指定槽位（服务器权威；越界/同槽/无武器时拒绝并返回 false） */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool RequestSwitchWeapon(int32 SlotIndex);

    /** 请求切到下一个槽位（循环；只在"多槽位"时有意义） */
    UFUNCTION(BlueprintCallable, Category = "Weapon|Switch")
    bool RequestNextWeapon();

    // -------------------------------------------------------------------------
    // 只读查询（BlueprintPure：HUD / Lua 表现层用）
    // -------------------------------------------------------------------------
    /** 当前武器定义（无武器时为空） */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    UGameWeaponDefinition* GetCurrentWeapon() const;

    /** 当前槽位下标（客户端在复制到达前会解析为 0，见 GetResolvedSlotIndex） */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    int32 GetCurrentSlotIndex() const;

    /** 槽位总数 */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    int32 GetSlotCount() const { return WeaponSlots.Num(); }

    /** 取指定槽位的武器定义（越界返回空） */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    UGameWeaponDefinition* GetWeaponAtSlot(int32 SlotIndex) const;

    /** 当前武器弹匣内的子弹数 */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    int32 GetAmmo() const;

    /** 当前武器备弹数 */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    int32 GetReserveAmmo() const;

    /** 当前武器弹匣容量（来自武器定义；无武器时为 0） */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    int32 GetMagazineSize() const;

    /** 是否正在换弹 */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    bool IsReloading() const { return bReloading; }

    /**
     * 换弹进度（0..1）：供 HUD 进度环与换弹动画定位使用。
     * 实现说明：不复制"服务器时间戳"（那要求两端时钟严格同步），而是在**本端**记录
     *           "进入换弹状态的本地时刻"，用它与本端当前时间做差；两端各自都能得到流畅进度。
     */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    float GetReloadProgress() const;

    /**
     * 当前武器是否全自动：Lua 输入层据此决定"按住是否持续请求开火"。
     * 注意：射速上限仍在服务器（客户端只决定节奏意愿），因此本值不构成作弊面。
     */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    bool IsCurrentWeaponAutomatic() const;

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：HUD / 音效 / Lua 表现层订阅）
    // -------------------------------------------------------------------------
    /** 当前武器发生变化 */
    UPROPERTY(BlueprintAssignable, Category = "Weapon")
    FOnGameWeaponChanged OnWeaponChanged;

    /** 弹药发生变化（扣弹、换弹完成都走它） */
    UPROPERTY(BlueprintAssignable, Category = "Weapon")
    FOnGameWeaponAmmoChanged OnAmmoChanged;

    /** 换弹状态发生变化 */
    UPROPERTY(BlueprintAssignable, Category = "Weapon")
    FOnGameWeaponReloadStateChanged OnReloadStateChanged;

    /** 已开火（各端表现入口） */
    UPROPERTY(BlueprintAssignable, Category = "Weapon")
    FOnGameWeaponFired OnFired;

    /** 空仓扣扳机（本地表现入口） */
    UPROPERTY(BlueprintAssignable, Category = "Weapon")
    FOnGameWeaponEmptyTriggered OnEmptyTriggered;

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    // -------------------------------------------------------------------------
    // 复制回调（客户端：把状态变化广播给表现层）
    // -------------------------------------------------------------------------
    UFUNCTION()
    void OnRep_CurrentSlotIndex();

    UFUNCTION()
    void OnRep_AmmoState();

    UFUNCTION()
    void OnRep_ReloadState();

    /**
     * 开火表现广播（**不可靠多播**）。
     *
     * 为什么用多播 RPC 而不是"复制一个递增的开火序号"：
     *  - 开火是高频瞬时事件，用属性递增会产生"数值本身无意义、只是触发信号"的怪状态，
     *    且新加入的客户端会因首次复制触发一次"虚假开火表现"；
     *  - 不可靠多播与 GameplayCue 的通道语义一致：表现允许丢弃（连续射击丢中间几次不影响观感），
     *    不占可靠带宽、不因堆积而延迟。
     */
    UFUNCTION(NetMulticast, Unreliable)
    void Multicast_Fired();

    /**
     * 服务器 → 开火者本端：开火确认（携带扣弹后的权威弹匣/备弹，定向只发开火者一人）。
     *
     * 为什么需要它（弹药预测的"对账"闭环）：
     *  客户端开火走"本地预测扣弹"（HUD 立即 -1，不等服务器复制）；
     *  若某次预测请求被服务器拒绝（节拍偏差超出容差的罕见情形），服务器弹药不变、
     *  属性复制不会到来，本地"少显示 1 发"的偏差将一直残留到下次换弹。
     *  本 RPC 在**每次服务器成功开火后**把权威值带给开火者：无论此前预测是否准确，
     *  收到即对齐 —— 彻底消除偏差残留。不可靠通道：丢了也有属性复制兜底，下一次确认会再次对齐。
     *  与属性复制的到达顺序无关紧要：两者携带的是同一份扣弹后的新值，重复赋值幂等。
     */
    UFUNCTION(Client, Unreliable)
    void ClientConfirmAmmo(int32 SlotIndex, int32 ConfirmedAmmo, int32 ConfirmedReserveAmmo);

private:
    /**
     * 本组件是否处于"权威端"。
     * 注意（引擎事实）：`HasAuthority()` 是 `AActor` 的方法，**`UActorComponent` 上没有**，
     * 组件必须经 Owner 判断；故统一收敛成这一个私有入口，避免各处重复写 `GetOwner()->...` 并漏判空 Owner。
     */
    bool HasWeaponAuthority() const;

    /** 解析可用槽位：CurrentSlotIndex 非法（如客户端复制尚未到达）时回退到 0 */
    int32 GetResolvedSlotIndex() const;

    /** 本地连发定时器回调：按射速节奏反复请求开火 */
    void RepeatFireRequestTimer();

    /**
     * 本地（拥有该 Pawn 的端）请求开火一次。
     * 内部：① 换弹中跳过请求（不停连发定时器）；② 客户端节拍预检（与服务器锁相同节拍，
     *      超频请求静默丢弃 —— 它们注定被服务器拒绝，且本地已预测扣弹会留下"幽灵弹匣"）；
     *      ③ 本地预判空仓 → 只播"空仓"表现不发请求；④ 客户端本地预测扣弹（HUD 立即 -1）；
     *      ⑤ 经 GAS 按开火标签激活开火技能（技能 → 服务器节拍锁相 → 组件结算）；
     *      ⑥ 在**本端**广播 OnFired 做即时表现（本地预测）。
     *
     * 说明：⑥ 是"客户端即时反馈"的必要代价 —— 若等服务器回包再播枪口火焰，玩家会感到明显延迟。
     *      预测与判定的一致性由"本地节拍预检 = 服务器锁相节拍 + 服务器容差吸收到达抖动"保证（见 TryFire）；
     *      万一个别请求仍被服务器拒绝，下一次 ClientConfirmAmmo 会把权威弹匣带回（见其注释）。
     *      服务器实际开火时，其他客户端由 Multicast_Fired 收到表现；开火者本人不会收到重复表现（见其实现）。
     */
    void RequestFireLocally();

    /**
     * 客户端本地预测扣弹：立即把指定槽的弹匣减一并广播（仅非权威端调用）。
     * HUD 因此零滞后；权威值随后经 ClientConfirmAmmo / 属性复制到达对齐。
     * 钳到 0：预测不允许出现负数（宁可少显示也不显示负弹匣）。
     */
    void PredictAmmoLocally(int32 Slot);

    /** 本地连发定时器的触发间隔（= 武器标称射速间隔；RPM <= 0 时返回 0 表示不启定时器） */
    float GetAutoFireRequestInterval() const;

    /** 初始化各槽弹药（服务器权威；客户端只做占位，随后会被服务器复制值整体覆盖） */
    void EnsureAmmoArraysInitialized();

    /** 服务器：选中初始槽位（并广播） */
    void SelectInitialSlotServer();

    /** 服务器：按武器定义结算一次射线射击（含霰弹多发） */
    void PerformHitscanServer(const UGameWeaponDefinition& Weapon, const FVector& AimLocation, const FRotator& AimRotation);

    /** 服务器：生成投射物 */
    void SpawnProjectileServer(const UGameWeaponDefinition& Weapon, const FVector& AimLocation, const FRotator& AimRotation);

    /** 服务器：完成换弹（定时器回调） */
    void FinishReloadServer();

    /** 服务器：中止换弹（切枪打断 / 卸下时清理） */
    void CancelReloadServer();

    /** 本端：记录"进入/离开换弹状态的本地时刻"（两端各自维护，供进度查询使用） */
    void MarkReloadStartLocalTime();

    /** 取本角色的 ASC（伤害施加需要它；走 IAbilitySystemInterface 解耦具体角色类） */
    class UGameAbilitySystemComponent* GetOwnerAbilitySystemComponent() const;

    /** 广播封装：弹药变化（服务器改值后必须手动广播 —— RepNotify 在服务器不会触发） */
    void NotifyAmmoChanged();

    /** 广播封装：当前武器变化 */
    void NotifyWeaponChanged();

    /** 广播封装：换弹状态变化 */
    void NotifyReloadStateChanged(bool bInReloading, float InReloadSeconds);

    /** 广播封装：已开火 */
    void NotifyFired();

private:
    // -------------------------------------------------------------------------
    // 复制状态（服务器权威）
    // -------------------------------------------------------------------------
    /** 当前槽位（RepNotify：客户端据此换枪表现） */
    UPROPERTY(ReplicatedUsing = OnRep_CurrentSlotIndex)
    int32 CurrentSlotIndex = INDEX_NONE;

    /**
     * 每槽弹匣内子弹数（数组整体复制）。
     * 用数组而非单值的原因：**每把枪各自保留弹药**（切枪回来弹药不变），这是竞技射击的必需语义；
     * 数组元素少（槽位通常 ≤ 4），整体复制的开销可忽略。
     */
    UPROPERTY(ReplicatedUsing = OnRep_AmmoState)
    TArray<int32> SlotAmmo;

    /** 每槽备弹数（同上） */
    UPROPERTY(ReplicatedUsing = OnRep_AmmoState)
    TArray<int32> SlotReserveAmmo;

    /** 是否正在换弹（RepNotify：客户端播放换弹动画/进度） */
    UPROPERTY(ReplicatedUsing = OnRep_ReloadState)
    bool bReloading = false;

    // -------------------------------------------------------------------------
    // 本地/服务器内部状态（不复制）
    // -------------------------------------------------------------------------
    /**
     * 服务器侧"下一个允许开火时刻"（秒）：节拍锁相（Phase-Locked）射速限速的核心状态，只有服务器读写。
     *
     * 工作方式（见 TryFire）：
     *  - 放行条件：Now >= 本值 - 容差（容差只吸收"请求到达"的定时器粒度与网络抖动）；
     *  - 放行后推进：本值 = max(本值 + 射速间隔, Now + 射速间隔)。
     *    刻意用"节拍累加"而不是"重置为 Now + 间隔"：
     *    若重置，作弊者把每次请求都贴在"允许时刻 - 容差"上，节拍基准会随之前移，
     *    射速被抬高约 容差/间隔（10ms 容差在 600RPM 下约 10%）；
     *    锁相累加下节拍基准独立于请求时刻推进 —— 作弊者贴边请求无法前移节拍，
     *    **长期平均射速恰为标称值，零收益**；正常玩家偶发早到抖动则被容差完全吸收，零损失。
     *    max 的第二支负责"晚到/首火重新同步"：停火很久后再开火，节拍直接从当前时刻重启，不产生补发。
     * 对比"滑动窗口限速"（记录最近 N 秒开火时间戳数组）：锁相方案 O(1) 状态、零分配、无常数级配额泄露，
     * 是更优解，故本组件不引入时间戳窗口。
     */
    float NextAllowedFireServerTime = -1000.0f;

    /**
     * 本端"上一次请求开火时刻"（秒）：客户端节拍预检依据，只有非权威端（纯客户端）读写。
     *
     * 为什么客户端也要做一道节拍闸（弹药预测的前置）：
     *  客户端开火走"本地预测扣弹"（见 RequestFireLocally），预测成立的前提是
     *  "本地放行的请求 ≈ 服务器也会放行的请求"。本地按标称节拍预检后，
     *  正常请求在服务器必然落在锁相容差窗口内被放行 → 预测值与权威值天然一致；
     *  若客户端不预检，高频连点的请求虽会被服务器拒绝，但本地已扣的弹无法逐发收回，
     *  HUD 会显示一个"幽灵弹匣"。
     */
    float LastLocalFireRequestTime = -1000.0f;

    /** 本端进入换弹状态的时刻（秒）：进度查询依据，两端各自维护 */
    float LocalReloadStartTime = 0.0f;

    /** 换弹完成定时器 */
    FTimerHandle ReloadTimerHandle;

    /** 本地"按住连发"定时器（仅在本端存在；由 StartFire/StopFire 起停） */
    FTimerHandle AutoFireTimerHandle;

    /**
     * 服务器侧"到达提前容差"（秒）。
     *
     * 为什么需要：客户端按标称射速发出的请求，其**到达服务器**的时刻会受客户端定时器粒度（帧粒度约 16ms）
     * 与网络抖动影响；若服务器要求严格 >= 射速间隔，玩家连发时会因几毫秒偏差被大量拒绝，
     * 表现为"枪时不时哑火"（本地已播表现、服务器没开火）。
     *
     * 反作弊收益（配合 NextAllowedFireServerTime 的锁相累加）：本容差只允许"单发早到"，
     * 节拍基准不会因贴边请求而前移 —— 作弊者长期平均射速恰为标称值，零收益；
     * 正常玩家偶发早到被完全吸收，零损失。（旧"间隔 - 容差"方案的 ~10% 作弊收益已被消除。）
     */
    static constexpr float ServerFireThrottleToleranceSeconds = 0.01f;

    /** 开发期：是否绘制开火射线（打包构建中 DrawDebug 会被编译剔除） */
    UPROPERTY(EditDefaultsOnly, Category = "Weapon|Debug")
    bool bDrawDebugTrace = false;
};
