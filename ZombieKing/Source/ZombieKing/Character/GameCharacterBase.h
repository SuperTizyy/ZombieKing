#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AbilitySystemInterface.h"
#include "GenericTeamAgentInterface.h"
#include "GameplayTagContainer.h"
#include "MoverSimulationTypes.h" // IMoverInputProducerInterface / FMoverInputCmdContext（Mover 输入指令管线）
#include "UnLuaInterface.h"
#include "CombatComponent.h"      // UCombatComponent / EHitZone（受击硬直事件的参数类型需要完整定义）
#include "GameCharacterBase.generated.h"

class UCharacterMoverComponent;
class UCombatComponent;
class UHealthComponent;
class UGameWeaponComponent;          // 武器组件（装备槽/弹药/换弹/切枪/开火机制）
class UCameraComponent;
class USpringArmComponent;
class UCapsuleComponent;
class UBoxComponent;                 // 宽扁体型的盒体根碰撞
class UGameCharacterDefinition;      // 角色定义数据资产（体型/阵营/技能集/初始属性/移动参数）
class USkeletalMesh;                 // 骨骼网格体（进化"模型小变化"的替换源；默认网格缓存需要该类型）
class UEnhancedInputComponent;       // Enhanced Input 组件（技能输入绑定）
class UInputMappingContext;
class UInputAction;

/** 受击硬直变化（角色本端广播）：表现层播硬直动画/音效；逻辑层可用 IsStaggered 查询移动是否受限 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameStaggerChanged, float, DurationSeconds, AActor*, InstigatorActor);

/** 命中确认（攻击者本端）：HUD 命中标记 / 命中音效 / 伤害数字 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnGameLocalHitConfirmed, AActor*, HitTarget, float, DamageAmount, bool, bHeadshot);

/** 击杀确认（击杀者本端）：击杀提示 / 击杀音效 / 连杀计数 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameLocalKillConfirmed, AActor*, VictimActor);

/**
 * 竞技生化网游通用角色基础容器（双足人类 / 多足生化幽灵共用）
 *
 * 设计特点：
 *  - 真第一人称：1P 与 3P 共用同一套骨骼 Mesh，本地隐藏头部骨骼但保留投影；
 *  - UnLua 脚本动态绑定：角色玩法逻辑 C++ 极薄，表现与技能请求下沉到 Lua；
 *  - Mover 2.0（UCharacterMoverComponent）：运动模式状态机驱动移动，
 *    双足行走（Walking/Falling）与多足爬墙（自定义 Climbing 模式）统一纳入预测/回滚管线；
 *  - 玩家输入经 IMoverInputProducerInterface 生成输入指令（InputCmd），
 *    相机朝向随输入指令一起复制到服务器并参与客户端重模拟，保证竞技对抗的一致性；
 *  - PlayerState 托管 GAS：Pawn 仅作为短生命周期 Avatar，感染变异/换人不断技能体系。
 */
UCLASS(Abstract)
class ZOMBIEKING_API AGameCharacterBase : public APawn,
                                           public IAbilitySystemInterface,
                                           public IGenericTeamAgentInterface,
                                           public IUnLuaInterface,
                                           public IMoverInputProducerInterface
{
    GENERATED_BODY()

public:
    AGameCharacterBase(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 告诉 UnLua 运行时，该 Actor 应该绑定哪个 Lua 文件路径（相对 Content/Script，如 "Lua.Core.Character.GameCharacterBase"） */
    // 注意：UnLua 2.3.6 的接口声明为 const（IUnLuaInterface::GetModuleName() const），签名必须一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // GAS 接口实现 (代理至 PlayerState，解耦 Pawn 变异销毁)
    // -------------------------------------------------------------------------
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

    // -------------------------------------------------------------------------
    // 阵营系统 (支持准星变色、友伤判定、生化人类区分)
    // -------------------------------------------------------------------------
    virtual void SetGenericTeamId(const FGenericTeamId& InTeamID) override;
    virtual FGenericTeamId GetGenericTeamId() const override;

    // -------------------------------------------------------------------------
    // Mover 输入生产者接口（官方 MoverExamples 范式）
    // 由 Mover 后端在"拥有该 Actor 的实例"（自主代理/服务器）上调用，
    // 用于生成本帧输入指令；输入指令会随移动管线的预测/回滚一起被重放。
    // -------------------------------------------------------------------------
    virtual void ProduceInput_Implementation(int SimTimeMs, FMoverInputCmdContext& InputCmdResult) override;

    // -------------------------------------------------------------------------
    // 运动能力查询（供 C++ / 蓝图 / UnLua 统一调用）
    // -------------------------------------------------------------------------
    /** 查询本角色是否具备指定的运动能力标签（如 GameCharacterTags::Capability_Climb） */
    UFUNCTION(BlueprintPure, Category = "Capability")
    bool HasMovementCapability(FGameplayTag CapabilityTag) const;

    // -------------------------------------------------------------------------
    // 武器查询（供 C++ / Lua / HUD 统一入口）
    // -------------------------------------------------------------------------
    /**
     * 取本角色的武器组件（未挂载时为空）。
     * 提供本入口的目的：Lua 侧需要一个稳定的取用点（脚本里不必自己去 FindComponentByClass），
     * 且 HUD 绑定弹药也能经它拿到统一的组件引用。
     * 注意：实现放在 .cpp —— 头文件里 UGameWeaponComponent 只有前向声明，
     *       返回裸指针的转换需要在完整类型可见处进行（避免依赖不完整类型的指针转换细节）。
     */
    UFUNCTION(BlueprintPure, Category = "Weapon")
    UGameWeaponComponent* GetWeaponComponent() const;

    // -------------------------------------------------------------------------
    // 受击硬直（状态查询 + 事件）
    // -------------------------------------------------------------------------
    /**
     * 当前是否处于受击硬直中。
     * 语义：硬直期间角色**不产生任何移动与跳跃意图**（见 OnProduceInput 的说明），
     *       因此本查询同时就是"我现在能不能动"的权威判断；表现层（动画/HUD）也可用它切状态。
     */
    UFUNCTION(BlueprintPure, Category = "Combat|Stagger")
    bool IsStaggered() const;

    /** 硬直剩余时长（秒）；未处于硬直时为 0 */
    UFUNCTION(BlueprintPure, Category = "Combat|Stagger")
    float GetStaggerRemainingSeconds() const;

    /** 受击硬直事件（携带本次硬直时长）——表现层据此播硬直动画/镜头抖动/音效 */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnGameStaggerChanged OnStaggerChanged;

    /**
     * 命中确认事件（**仅攻击者本端**收到）：由服务器确认"你打中了 X"后广播。
     * 用途：HUD 命中标记、命中音效、伤害数字。
     */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnGameLocalHitConfirmed OnHitConfirmed;

    /** 击杀确认事件（**仅击杀者本端**收到）：由服务器确认击杀成立后广播 */
    UPROPERTY(BlueprintAssignable, Category = "Combat")
    FOnGameLocalKillConfirmed OnKillConfirmed;

    /**
     * 最近一次受击是否命中头部（供"击杀播报是否标爆头"使用）。
     * 说明：爆头判定只在受击时才有（死亡事件本身不带命中细节），故在此缓存一份：
     *       死亡路径（Lua 规则）可据此把"是否爆头击杀"一并写入击杀播报。
     */
    UFUNCTION(BlueprintPure, Category = "Combat")
    bool WasLastHitHeadshot() const { return bLastHitWasHeadshot; }

    /**
     * 最近一次受击是否为近战（刀/爪）命中（供 Lua 死亡规则实现 CF 式"刀杀禁令"使用）。
     * 说明：与爆头缓存同理 —— 死亡事件不带命中细节，近战结论在受击时由
     *       CombatComponent 从伤害规格的资产标签读出并随事件携带，此处缓存。
     *       只有服务器侧的读取有意义（死亡处置规则跑在服务器）。
     */
    UFUNCTION(BlueprintPure, Category = "Combat")
    bool WasLastHitMelee() const { return bLastHitWasMelee; }

    // -------------------------------------------------------------------------
    // 生化幽灵进化（等级状态在 PlayerState；本类负责"升级落地"：血量增量 + 体型缩放 + 模型替换）
    // -------------------------------------------------------------------------
    /**
     * 设置本角色的进化等级（仅服务器生效；Lua 在"变身完成 / 感染数达标升级"时调用）。
     * 内部链路：
     *  1) 写 PlayerState 等级状态（复制 + 广播 → 各端角色订阅事件刷新体型/模型表现）；
     *  2) 服务器施加"进化属性增量"GE（按定义资产逐级表累加 MaxHealthDelta，升级即回等额血）；
     *  3) 幂等：同一 Avatar 已应用过的等级不会重复施加（防 Lua 重复调用与复活恢复双算）。
     * @param NewLevel 目标等级（1..6；0 = 归零回人类形态，等级状态清零）
     */
    UFUNCTION(BlueprintCallable, Category = "ZombieEvolution")
    void SetZombieLevel(int32 NewLevel);

    // -------------------------------------------------------------------------
    // 瞄准朝向（复制；供观战相机与远端渲染使用）
    // -------------------------------------------------------------------------
    /**
     * 取本角色"控制者的瞄准朝向"的复制值（服务器在 PreReplication 里写入）。
     *
     * 为什么需要自己复制一份：
     *  - 引擎只复制了 APawn::RemoteViewPitch16（俯仰），偏航默认假定"等于 Actor 朝向"；
     *  - 但本项目角色不随控制器偏航旋转（APawn::bUseControllerRotationYaw 为默认 false），
     *    Actor 朝向只反映移动朝向，因此远端客户端与观战者拿不到"他正在看哪里"；
     *  - 而 AController::ControlRotation 本身是**非复制**字段，观战端那份 PC 上恒为 0，
     *    直接读它只会得到完全错误的方向。
     * 结论：把"控制旋转"整体复制下来，是观战第一人称跟随与远端瞄准表现的正确数据源。
     */
    UFUNCTION(BlueprintPure, Category = "Camera")
    FRotator GetReplicatedAimRotation() const { return ReplicatedAimRotation; }

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent，UnLua 绑定脚本同名函数自动接管）
    // C++ 只负责派发原始输入事件，具体表现（动画/音效/技能请求）全部在 Lua 处理。
    // -------------------------------------------------------------------------
    /** 移动输入（Forward/Right 为 -1..1 的原始轴值，仅用于表现层，不参与移动计算） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputMove(float ForwardAxis, float RightAxis);

    /** 视角输入（Yaw/Pitch 原始轴值，仅用于表现层） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputLook(float YawAxis, float PitchAxis);

    /** 攻击输入（技能激活已由 C++ 按输入标签交给 GAS；本事件仅供 Lua 播放第一人称攻击表现） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputAttack();

    /** 跳跃输入（Lua 中处理本地跳跃表现；权威位移由 Mover 输入指令处理） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputJump();

    /** 进出爬墙状态变化（多足角色吸附/脱离墙面时广播，Lua 接管镜头/后处理/音效表现） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Movement")
    void OnClimbStateChanged(bool bEnterClimb);

    /** 技能输入按下（Lua 接管按键反馈/音效/准星等表现；权威技能激活已在 C++ 交给 GAS） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputAbility(FGameplayTag InputTag);

    /**
     * 开火输入（按下）。
     * 分工：C++ 只转发给 Lua；真正的开火请求由 Lua 调武器组件执行
     *      （UGameWeaponComponent::StartFire 内部按射速节奏反复请求开火，且做了服务器权威节流）。
     * 为什么开火要有独立键位通道而不复用"技能输入标签"通道：
     *  ① 射击必须区分"按下/松开"（按住连发、松手停火），而技能输入通道只表达"按下"；
     *  ② 开火键不需要配进 UGameInputConfig（它不激活"任意技能"，只激活武器开火技能），
     *     独立通道可以少一处内容配置、少一处出错点。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputFireStart();

    /** 开火输入（松开）：Lua 据此停火（停止连发循环） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputFireRelease();

    /**
     * 换弹输入（R 键等）。
     * 分工：C++ 只把原始按键事件转发给 Lua；真正的换弹请求由 Lua 调武器组件执行
     *      （UGameWeaponComponent::RequestReload 内部做服务器权威校验）。
     * 为什么不在 C++ 直接调组件：换弹是否需要附加前置规则（如爬墙中禁止换弹、换弹时打断某个动作）
     * 属**规则**，规则一律留在 Lua（可热更），C++ 只做"按键 → 事件"的机械转发。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputReload();

    /** 切枪输入（Q 键等）：同上，C++ 只转发，Lua 调武器组件请求切换 */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputSwitchWeapon();

    /**
     * 本角色"已拿到控制器"（服务器在 PossessedBy、客户端在 OnRep_Controller 时派发）。
     * 用途：需要"控制权就绪"后才做的表现 —— 典型场景是"死亡时视角被切到击杀者（复仇镜头），
     *       复活/重连拿到新角色后要把视角切回自己"，否则会出现"控制着新角色却看着别人"。
     * 之所以单独设一个事件而不是复用 PossessedBy：客户端的 Pawn 不会走 PossessedBy（那是服务器路径），
     * 客户端的可靠钩子是 OnRep_Controller —— 两者都由 C++ 统一转成本事件，Lua 只需实现一次。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Lifecycle")
    void OnControllerReady();

    /**
     * 受击事件（BlueprintNativeEvent）。
     * 由 UCombatComponent 在收到"受伤"后派发；命中部位与攻击来源随事件一同送达。
     * 契约：Lua 侧以 OnDamaged_Implementation(DamageAmount, HitBone, InstigatorActor) 实现表现
     *      （见 Content/Script/Lua/Core/Character/GameCharacterBase.lua，该文件已按此签名预留）。
     * @param DamageAmount     本次受击伤害（正数）
     * @param HitBone          命中的骨骼名（范围伤害/无骨骼信息时为 NAME_None）
     * @param InstigatorActor  攻击来源（用于受击方向指示，可为空）
     */
    UFUNCTION(BlueprintNativeEvent, Category = "Lua|Damage")
    void OnDamaged(float DamageAmount, FName HitBone, AActor* InstigatorActor);
    virtual void OnDamaged_Implementation(float DamageAmount, FName HitBone, AActor* InstigatorActor);

    /**
     * 死亡事件（BlueprintNativeEvent）。
     * 由 UHealthComponent 在生命首次归零时派发；Lua 侧以 OnDeath_Implementation(KillerActor) 实现死亡表现
     * （肉体松弛/碎裂特效/死亡音效等——注意本回调返回后，死亡规则（复活或转观察者）会由 GameMode 执行，
     *   若表现需要在角色销毁前完成，请在此同步处理）。
     * @param KillerActor 击杀者（未知则为空）
     */
    UFUNCTION(BlueprintNativeEvent, Category = "Lua|Damage")
    void OnDeath(AActor* KillerActor);
    virtual void OnDeath_Implementation(AActor* KillerActor);

    // -------------------------------------------------------------------------
    // 核心组件挂载
    // -------------------------------------------------------------------------
    /**
     * 根碰撞胶囊体（Mover 位移模拟的实际载体）。
     * 蓝图可随意调整尺寸：在蓝图子类的 Components 面板选中本组件 → Details → Shape →
     * Capsule Half Height / Capsule Radius（两个字段本身是 EditAnywhere，可自由修改）。
     * 引擎硬约束：CapsuleHalfHeight >= CapsuleRadius（半球不可重叠），半径超限会被自动钳回；
     * 若需要真正"宽扁"的碰撞底盘，应改用 UBoxComponent 作为根组件。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
    TObjectPtr<UCapsuleComponent> CollisionCapsule;

    /**
     * 盒体形态的根碰撞体（仅"宽扁体型"角色创建，由子类构造函数调用 SetBoxCollisionRoot 生成）。
     * 未切换盒体形态的角色此成员为空，属正常状态；切换后胶囊会被降级为盒体的子组件并停用碰撞。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Collision")
    TObjectPtr<UBoxComponent> CollisionBox;

    /** 角色三维骨骼表现网格体（真第一人称方案：1P 与 3P 共用此一套 Mesh，不参与碰撞） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Mesh")
    TObjectPtr<USkeletalMeshComponent> CharacterMesh;

    /**
     * Mover 运动组件（UCharacterMoverComponent：自带 Walking/Falling/Flying 模式、落地检测、跳跃处理）。
     * 多足生化角色会在子类构造函数中通过 SetDefaultSubobjectClass 替换为更专门的
     * UCrawlerMoverComponent（追加爬墙模式与吸附过渡），基类保持双足人类通用配置。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Movement")
    TObjectPtr<UCharacterMoverComponent> MoverComponent;

    /** 战斗逻辑组件（受击硬直计算、部位判定、反作弊验证回执） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Combat")
    TObjectPtr<UCombatComponent> CombatComponent;

    /** 生命状态组件（监听 GAS 生命属性扣减、死亡生命周期） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
    TObjectPtr<UHealthComponent> HealthComponent;

    /**
     * 武器组件（武器系统的**服务器权威**所在）：装备槽、每槽弹药、换弹状态、切枪、开火机制。
     * 说明：基类对所有角色都挂载它（人类配枪、生化可留空或配"投掷类"武器定义），
     *       这样"能不能开火"始终由同一套机制裁决，不需要为不同阵营写两套武器逻辑。
     *       武器数值全在 UGameWeaponDefinition 资产里，本组件只提供机制（见其类注释）。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon")
    TObjectPtr<UGameWeaponComponent> WeaponComponent;

    /** 第一人称相机组件（挂载于头部骨骼或弹簧臂） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<UCameraComponent> FirstPersonCamera;

    /** 短弹簧臂：用于微调相机旋转与头部震动吸收，避免硬绑定导致的剧烈抖动 */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Camera")
    TObjectPtr<USpringArmComponent> CameraBoom;

    // -------------------------------------------------------------------------
    // 运行时配置
    // -------------------------------------------------------------------------
    /** Lua 脚本模块路径（相对 Content/Script），允许在蓝图子类中直接覆盖配置 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

    // -------------------------------------------------------------------------
    // 角色定义（体型 / 阵营 / 技能集 / 初始属性 / 移动参数）—— 数据驱动
    // -------------------------------------------------------------------------
    /**
     * 本角色的定义资产（大厂的 "PawnData" 模式）：一个角色类型对应一份资产。
     * 体型、阵营、技能集、初始属性 GE、移动参数全部来自该资产；
     * 角色类只负责"结构性能力"（双足/多足、是否挂载爬墙 Mover 组件）。
     * 因此这些配置的调整全部是内容改动，无需重新编译 C++（满足竞技网游热更新要求）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Definition")
    TObjectPtr<UGameCharacterDefinition> CharacterDefinition;

    // -------------------------------------------------------------------------
    // 运动能力标签（双足 / 多足 / 爬墙）
    // -------------------------------------------------------------------------
    /**
     * 本角色具备的运动能力标签集合，由子类在构造函数中声明
     * （例如：人类 = Biped；可爬墙的多足幽灵 = Multiped + Climb）。
     * 采用 GameplayTags 而非布尔字段：能力可组合，且可被 AI / UI / 规则脚本(Lua) 用同一套标签体系统一查询。
     * 注意：刻意用 VisibleAnywhere（只读展示）而非 EditDefaultsOnly——
     *       避免蓝图子类把该容器"另存为默认值"后覆盖掉 C++ 构造函数里声明的能力标签。
     */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Capability")
    FGameplayTagContainer MovementCapabilityTags;

    // -------------------------------------------------------------------------
    // 技能集说明
    // -------------------------------------------------------------------------
    // 本角色的技能不再放在角色类上，而由 CharacterDefinition->AbilitySets 提供
    // （大厂做法：技能集是可复用的内容资产，可被多个角色类型共享，见 UGameAbilitySet）。

    /** 本地第一人称视角下需要隐藏的头部骨骼名称（防止自己视线被自身面部模型遮挡） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera|TrueFPS")
    FName HeadBoneName;

    // -------------------------------------------------------------------------
    // 通用输入配置（Enhanced Input，各角色蓝图在 Class Defaults 中指定自己的资产）
    // 组织说明（对齐 Epic 官方 Mover 参考实现 MoverExamplesCharacter 的基类组织方式）：
    //  - "移动/视角/跳跃/攻击"为双足人类与多足生化共用的基础输入通道，统一在基类声明与绑定；
    //  - 各角色蓝图只需指定各自的 IMC/IA 资产，无需重复编写按键绑定与缓存代码；
    //  - 子类需要额外按键（技能、换弹、蹲伏等）时，重写 SetupPlayerInputComponent 并在
    //    调用 Super 后追加绑定即可。
    // -------------------------------------------------------------------------
    /** 默认输入映射上下文（IMC：本角色的按键映射组合） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputMappingContext> DefaultMappingContext;

    /** 移动输入动作（Axis2D：X = 左右，Y = 前后） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionMove;

    /** 视角输入动作（Axis2D：X = 偏航，Y = 俯仰） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionLook;

    /** 攻击输入动作（Started 触发，转发 Lua 技能请求） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionAttack;

    /**
     * 开火输入动作（可选；Started=按下、Completed=松开两事件都绑）。
     * 与 ActionAttack 的关系：ActionAttack 是"通用攻击"（近战爪击等，走技能输入标签通道）；
     * 本动作是"武器开火"（走武器组件的连发机制），两者可同时存在（例如持枪角色也有近战）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionFire;

    /** 跳跃输入动作（Started/Completed 双事件：瞬时按下与持续按压分开建模） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionJump;

    /**
     * 换弹输入动作（可选）。
     * 未填则跳过绑定（生化幽灵等"没有武器"的角色无需配置，不产生告警噪音）。
     */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionReload;

    /** 切枪输入动作（可选，语义为"切到下一把"；数字键直选槽位由内容侧按需扩展） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Input")
    TObjectPtr<UInputAction> ActionNextWeapon;

protected:
    virtual void BeginPlay() override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void OnRep_PlayerState() override;
    virtual void UnPossessed() override;
    virtual void OnRep_Controller() override;

    /**
     * 复制声明：受击硬直是"服务器权威状态"（受击判定只在服务器有意义），
     * 但客户端必须知道"我正在硬直"才能在生产输入时削减移动意图（见 OnProduceInput），
     * 故把硬直时长复制下来（客户端在 OnRep_StaggerState 里起本地倒计时）。
     */
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /**
     * 复制前钩子（仅服务器）：把"控制者的控制旋转"写入复制字段 ReplicatedAimRotation。
     * 放这里而不是逐帧 Tick 写：PreReplication 由网络层在真正需要发送前调用，
     * 只在字段变化时才产生网络开销（普通复制字段的变化检测由引擎完成）。
     * 与引擎自身在 APawn::PreReplication 里写 RemoteViewPitch 的做法完全一致。
     */
    virtual void PreReplication(IRepChangedPropertyTracker& ChangedPropertyTracker) override;

    // -------------------------------------------------------------------------
    // 受击硬直：事件桥接与本地倒计时
    // -------------------------------------------------------------------------
    /** CombatComponent 硬直事件的桥接回调（服务器：写入复制字段 + 起本地倒计时 + 广播） */
    UFUNCTION()
    void HandleCombatStaggered(float Duration, EHitZone HitZone, AActor* InstigatorActor);

    /** 硬直状态复制回调（客户端：拿到服务器给的时长后开始本地倒计时并广播） */
    UFUNCTION()
    void OnRep_StaggerState();

    /** 起（或重设）本地硬直倒计时；bBroadcast 决定是否广播 OnStaggerChanged */
    void ApplyStaggerLocal(float DurationSeconds, AActor* InstigatorActor, bool bBroadcast);

    // -------------------------------------------------------------------------
    // 命中 / 击杀确认：ASC 事件 → 角色事件（供 Lua / HUD 统一订阅）
    // -------------------------------------------------------------------------
    /**
     * 为什么要在角色上再桥接一层：Lua 侧没有一个稳定的"取 ASC"入口
     * （引擎未把模板版 GetSubsystem/GetAbilitySystemComponent 暴露给 Lua），
     * 而角色本身是 UnLua 绑定对象，订阅角色的委托是最稳的路径。
     */
    UFUNCTION()
    void HandleASC_HitConfirmed(AActor* HitTarget, float DamageAmount, bool bHeadshot);

    /** ASC 击杀确认的桥接回调 */
    UFUNCTION()
    void HandleASC_KillConfirmed(AActor* VictimActor);

    // -------------------------------------------------------------------------
    // 通用输入绑定与回调（Enhanced Input → 缓存 → 输入指令）
    // -------------------------------------------------------------------------
    /** 绑定通用输入动作；子类扩展额外按键时重写并调用 Super */
    virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

    /** 移动输入：缓存本地轴意图（-1..1）并转发 Lua 表现层 */
    virtual void Input_Move(const struct FInputActionValue& Value);

    /**
     * 移动输入（松手）：清零缓存意图。
     * 关键说明：Enhanced Input 在"最后一个按键松开"时只保证派发 Completed 事件，
     * 不保证再派发零值 Triggered；而 Mover 的 DirectionalIntent 会按输入指令持续全速移动
     * （无地面摩擦自动停车），不显式清零就会出现"松手后角色一直朝一个方向漂移"。
     */
    virtual void Input_MoveCompleted(const struct FInputActionValue& Value);

    /** 视角输入：驱动 Controller 旋转（相机世界坐标方案，爬墙时镜头不翻转） */
    virtual void Input_Look(const struct FInputActionValue& Value);

    /** 攻击输入：转发 Lua（GAS 技能激活请求 + 第一人称攻击表现） */
    virtual void Input_Attack();

    /** 开火输入（按下）：只转发 Lua（开火请求由 Lua 调武器组件的 StartFire 执行） */
    virtual void Input_FireStart();

    /** 开火输入（松开）：只转发 Lua（停火） */
    virtual void Input_FireRelease();

    /** 跳跃输入：置位跳跃标志（模拟帧消费）+ 转发 Lua 表现 */
    virtual void Input_Jump();

    /** 跳跃释放：清除持续按压标志 */
    virtual void Input_JumpReleased();

    /** 换弹输入：只转发 Lua（换弹请求由 Lua 调武器组件执行，见 OnInputReload 注释） */
    virtual void Input_Reload();

    /** 切枪输入：只转发 Lua */
    virtual void Input_SwitchWeapon();

    // -------------------------------------------------------------------------
    // 技能输入绑定（输入动作 → 技能标签 → 按标签激活技能）
    // -------------------------------------------------------------------------
    /**
     * 绑定角色定义中"技能输入配置"（UGameInputConfig）里的全部输入动作。
     * 输入标签作为附加参数直接绑进回调，因此新增技能输入无需新增任何 C++ 回调函数。
     */
    void BindAbilityInputActions(UEnhancedInputComponent* EnhancedInput);

    /** 技能输入按下回调：按标签激活技能（GAS），并转发 Lua 表现（Value 未使用，保留标准签名） */
    void Input_AbilityInputTagPressed(const struct FInputActionValue& Value, FGameplayTag InputTag);

    /**
     * 输入指令生成的核心实现（子类可重写扩展，例如追加自定义输入数据块）。
     * 注意：此函数在"输入生产端"（本地自主代理/服务器）执行，位于移动模拟之外；
     * 产出的数据会随输入指令复制到远端并参与重模拟，因此这里只应生成输入数据，
     * 严禁修改任何权威世界状态（那些必须放进 MovementMode 的模拟回调里）。
     */
    virtual void OnProduceInput(float DeltaMs, FMoverInputCmdContext& OutInputCmd);

    // -------------------------------------------------------------------------
    // 输入缓存（由具体角色的 Enhanced Input 回调写入，OnProduceInput 消费）
    // -------------------------------------------------------------------------
    /** 移动意图缓存（X = 前后, Y = 左右, 取值 -1..1；在 OnProduceInput 中按相机朝向换算为世界空间） */
    FVector CachedMoveInputIntent = FVector::ZeroVector;

    /** 跳跃按钮持续按住状态 */
    bool bIsJumpPressed = false;

    /** 跳跃按钮"本帧刚按下"瞬时标志（被 OnProduceInput 消费后清零） */
    bool bIsJumpJustPressed = false;

    /**
     * 硬直时长（服务器权威，复制给客户端）。
     * 单位秒；由 CombatComponent 按命中部位查表得到（部位/时长只在服务器判定）。
     */
    UPROPERTY(ReplicatedUsing = OnRep_StaggerState, BlueprintReadOnly, Category = "Combat|Stagger")
    float ReplicatedStaggerSeconds = 0.0f;

    /**
     * 复制的"控制者瞄准朝向"（服务器权威写入，见 PreReplication）。
     * 供观战相机（第一人称跟随）与远端客户端的瞄准/持枪表现使用。
     */
    UPROPERTY(Replicated, BlueprintReadOnly, Category = "Camera")
    FRotator ReplicatedAimRotation = FRotator::ZeroRotator;

    /**
     * 本地硬直倒计时结束时刻（秒，不复制）。
     * 两端各自维护：服务器在受击时按本端时间起算，客户端在 OnRep 时按本端时间起算。
     * 之所以不复制"服务器时间戳"：那要求严格的跨机时钟同步，而硬直只影响"输入意愿"，
     * 毫秒级偏差不会造成可感知的不一致（权威结算仍在服务器）。
     */
    float LocalStaggerEndTime = 0.0f;

    /** 最近一次受击是否为头部命中（由 HandleCombatDamaged 刷新；供击杀播报标爆头） */
    bool bLastHitWasHeadshot = false;

    /** 最近一次受击是否为近战命中（由 HandleCombatDamaged 刷新；供 Lua"刀杀禁令"判定，仅服务器侧读取有意义） */
    bool bLastHitWasMelee = false;

    /**
     * 本 Avatar 已应用过进化属性增量的最高等级（不复制、每份新 Avatar 从 1 起算）。
     * 用途：进化 GE 的幂等保护 —— 复活换 Avatar 后由 SetupAbilitySystemContext 按
     *       PlayerState 等级一次性补算增量，Lua 重复调用同一等级不会二次加血。
     */
    int32 AppliedEvolutionLevel = 1;

    /** 蓝图默认骨骼网格体（BeginPlay 缓存一次；等级归 0 / 缺配置时用它还原"模型小变化"） */
    TWeakObjectPtr<USkeletalMesh> DefaultCharacterSkeletalMesh;

    /** 配置真第一人称网格体投射阴影与头部自遮挡裁剪 */
    void SetupTrueFirstPersonMesh();

    /** 构建 GAS 上下文信息，将 Actor 代理赋予长期存活的 PlayerState */
    void SetupAbilitySystemContext();

    /**
     * PlayerState 进化等级变化事件的桥接回调（UFUNCTION 以满足动态委托绑定要求；各端都会收到）。
     * 只做"本地表现"（体型缩放 + 模型替换）；属性增量只在服务器由 SetZombieLevel 显式施加，
     * 刻意不在这里补，避免客户端"自加血量"与复活恢复路径重复计算。
     */
    UFUNCTION()
    void HandleZombieLevelChanged(int32 NewLevel);

    /** 按等级应用"体型缩放 + 骨骼网格体替换"（各端本地执行，数值源=同一份角色定义资产，无需复制） */
    void ApplyZombieLevelVisuals(int32 Level);

    /**
     * 服务器：施加 (FromLevelExclusive, ToLevel] 区间的进化属性增量（一次 GE 合并累加值）。
     * 增量 = 角色定义 ZombieLevels 表内对应级的 MaxHealthDelta 之和；
     * 缺失级/未配进化 GE → 告警降级，绝不崩。
     */
    void ApplyEvolutionAttributeDelta(int32 FromLevelExclusive, int32 ToLevel);

    /**
     * CombatComponent 受击事件的桥接回调（UFUNCTION 以满足动态委托绑定要求）。
     * 作用：把组件事件转发到角色的 OnDamaged（BlueprintNativeEvent → Lua 表现），
     *       从而让 CombatComponent 不必依赖具体角色类，保持组件解耦。
     */
    UFUNCTION()
    void HandleCombatDamaged(float DamageAmount, FName HitBone, AActor* InstigatorActor, bool bMeleeHit);

    /**
     * HealthComponent 死亡事件的桥接回调（UFUNCTION 以满足动态委托绑定要求）。
     * 作用：① 派发角色自身的 OnDeath（Lua 死亡表现）；
     *       ② 在服务器把死亡上报给 GameMode，由 Lua 规则脚本决定"僵尸延迟复活 / 人类转观察者"。
     * 之所以"表现先、上报后"：规则可能会立即销毁死亡 Avatar，而表现需要在角色仍有效时同步执行。
     */
    UFUNCTION()
    void HandleHealthDeath(AActor* KillerActor);

    // -------------------------------------------------------------------------
    // 体型与碰撞形态
    // -------------------------------------------------------------------------
    /**
     * 把 BodyProfile 的尺寸灌入根碰撞体与表现网格体（在 BeginPlay 调用）。
     * 服务器与各客户端持有同一份 EditDefaultsOnly 资产，本地计算结果天然一致，故尺寸无需走网络复制。
     */
    void ApplyBodyProfile();

    /**
     * 供子类构造函数调用：把根碰撞体由胶囊切换为盒体（用于"宽扁"体型底盘）。
     * 引擎限制说明：Mover 的 UpdatedComponent 取自 RootComponent，因此宽扁体型角色必须在
     * 构造期就让盒体成为根组件；切换后原胶囊会自动挂到盒体之下，本函数同时停用其碰撞避免重复判定。
     * @param InHalfExtent 盒体半尺寸（X=前后, Y=左右, Z=上下，单位 cm）
     */
    void SetBoxCollisionRoot(const FVector& InHalfExtent);

    /**
     * 把角色定义中的移动参数（行走速度 / 跳跃高度）灌入 Mover 的运动配置（在 BeginPlay 调用）。
     * 说明：Mover 的行进速度与跳跃初速由"共享 Settings 对象"（UCommonLegacyMovementSettings）统一提供，
     *       多个运动模式共用它，因此改写它即可让所有地面/空中模式同时生效。
     * 子类若有额外运动模式参数（如爬墙速度），重写本函数并在调用 Super 后追加即可（见 AZombieCrawlerCharacter）。
     */
    virtual void ApplyMovementParams();
};
