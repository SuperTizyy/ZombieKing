#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AbilitySystemInterface.h"
#include "GenericTeamAgentInterface.h"
#include "GameplayTagContainer.h"
#include "MoverSimulationTypes.h" // IMoverInputProducerInterface / FMoverInputCmdContext（Mover 输入指令管线）
#include "UnLuaInterface.h"
#include "GameCharacterBase.generated.h"

class UCharacterMoverComponent;
class UCombatComponent;
class UHealthComponent;
class UCameraComponent;
class USpringArmComponent;

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
    virtual void ProduceInput_Implementation(int32 SimTimeMs, FMoverInputCmdContext& InputCmdResult) override;

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

    /** 攻击输入（Lua 中发起 GAS 技能激活请求与第一人称攻击表现） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputAttack();

    /** 跳跃输入（Lua 中处理本地跳跃表现；权威位移由 Mover 输入指令处理） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Input")
    void OnInputJump();

    /** 进出爬墙状态变化（多足角色吸附/脱离墙面时广播，Lua 接管镜头/后处理/音效表现） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Movement")
    void OnClimbStateChanged(bool bEnterClimb);

    // -------------------------------------------------------------------------
    // 核心组件挂载
    // -------------------------------------------------------------------------
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

    /** 生命状态组件（监听 GAS 属性扣减、护甲、死亡生命周期） */
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Health")
    TObjectPtr<UHealthComponent> HealthComponent;

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

    /** 本地第一人称视角下需要隐藏的头部骨骼名称（防止自己视线被自身面部模型遮挡） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Camera|TrueFPS")
    FName HeadBoneName;

protected:
    virtual void BeginPlay() override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void OnRep_PlayerState() override;
    virtual void UnPossessed() override;
    virtual void OnRep_Controller() override;

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

    /** 配置真第一人称网格体投射阴影与头部自遮挡裁剪 */
    void SetupTrueFirstPersonMesh();

    /** 构建 GAS 上下文信息，将 Actor 代理赋予长期存活的 PlayerState */
    void SetupAbilitySystemContext();
};
