#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "UnLuaInterface.h"
#include "GameAnimInstanceBase.generated.h"

class AGameCharacterBase;
class UCharacterMoverComponent;
class UCommonLegacyMovementSettings;
class UAnimSequenceBase;
class UBlendSpace;

/**
 * 竞技生化网游：通用动画实例基类（双足人类 / 多足生化角色共用）
 *
 * 架构定位（对齐项目 C++/UnLua 分层规范）：
 *  - 本类只做"引擎数据桥接"：每帧从 Mover 同步状态提取速度/朝向/落地/模式名等
 *    动画图必需数据，不做任何表现决策——表现决策位于动画图（ABP）状态机；
 *  - 热更新边界：可热更新的功能全部下沉 Lua（IUnLuaInterface 绑定的动画脚本）——
 *    速度阈值/播放速率参考值等调参在 Lua 的 OnAnimInstanceInitialized 中下发，
 *    模式变化的表现切换在 OnAnimMovementModeChanged 中处理，改完即生效；
 *    本类只保留引擎桥接与兜底默认值；
 *  - 性能红线：NativeUpdateAnimation 每帧执行，严禁在其中调用 Lua（每帧 Lua
 *    call 在竞技同屏场景代价不可接受），Lua 事件仅在初始化与模式变化边沿派发；
 *  - 动画资产槽位（Idle/混合空间/腾空循环）在此声明，具体角色的动画蓝图在
 *    Class Defaults 中覆盖赋值（"父逻辑 + 子资产覆盖"范式）；动画图不写死任何
 *    资产引用、不做骨骼特定修改（Epic 官方限制，保证父子链可跨骨架复用）；
 *  - 多线程安全：全部状态属性为 UAnimInstance 普通 UPROPERTY，动画图"多线程动画
 *    更新"下由引擎的属性拷贝机制在游戏线程完成快照，工作线程读取安全。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UGameAnimInstanceBase : public UAnimInstance, public IUnLuaInterface
{
    GENERATED_BODY()

public:
    UGameAnimInstanceBase();

    // -------------------------------------------------------------------------
    // UnLua 绑定接口（与 AGameCharacterBase 相同的动态脚本绑定范式）
    // -------------------------------------------------------------------------
    /** 告诉 UnLua 运行时该动画实例绑定哪个 Lua 文件路径（相对 Content/Script） */
    // 注意：UnLua 2.3.6 的接口声明为 const（IUnLuaInterface::GetModuleName() const），签名必须一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // 引擎回调
    // -------------------------------------------------------------------------
    virtual void NativeInitializeAnimation() override;
    virtual void NativeUpdateAnimation(float DeltaSeconds) override;

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent，UnLua 绑定脚本同名函数自动接管）
    // -------------------------------------------------------------------------
    /**
     * 动画实例初始化完成（派发于 NativeInitializeAnimation 末尾，仅一次）。
     * 热更新调参入口：Lua 在此覆盖 MovingSpeedThreshold / LocomotionReferenceSpeed 等，
     * 上线后可通过 Lua 热补丁调整步频、停车阈值等，无需重新打包资源。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Animation")
    void OnAnimInstanceInitialized();

    /**
     * Mover 运动模式变化（在 NativeUpdateAnimation 中检测模式名边沿后派发，仅变化时触发）。
     * Lua 层据此做进出爬墙/腾空/落地等模式的表现切换（音效/特效/镜头）。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Animation")
    void OnAnimMovementModeChanged(FName PreviousModeName, FName NewModeName);

    // -------------------------------------------------------------------------
    // 每帧动画数据（NativeUpdateAnimation 刷新，动画图用 Get 节点直接读取；
    // 所有数据取自 Mover 同步状态——预测/回滚一致，服务器与客户端表现统一）
    // -------------------------------------------------------------------------

    /** 世界空间速度（cm/s，Mover 同步状态权威值） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Velocity")
    FVector WorldVelocity = FVector::ZeroVector;

    /** 机身平面速度（cm/s；贴墙爬行时即"沿墙面平面"速度，双足/多足统一语义） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Velocity")
    float Speed = 0.0f;

    /** 机身坐标系 Z 轴速度（cm/s；贴墙时 = 垂直墙面方向分量，供贴墙姿态/蹬墙表现使用） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Velocity")
    float VelocityZ = 0.0f;

    /** 归一化机身平面移动方向（X=前后，Y=左右；静止时为零向量），供方向混合空间输入 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Velocity")
    FVector2D VelocityLocalDir = FVector2D::ZeroVector;

    /** 是否处于移动状态（Speed > MovingSpeedThreshold，阈值可由 Lua 热更调参） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    bool bIsMoving = false;

    /** 是否贴地（Mover：Walking 等地面模式） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    bool bIsOnGround = false;

    /** 是否腾空（Mover：Falling/Flying，爬墙模式两者皆非） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    bool bIsAirborne = false;

    /** 是否处于下落状态（引擎语义：含跳跃上升段） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    bool bIsFalling = false;

    /** 是否处于飞行模式 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    bool bIsFlying = false;

    /** 当前 Mover 运动模式名（Walking/Falling/Flying/自定义模式名，如多足爬墙 Climbing） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|State")
    FName MovementModeName;

    /** 角色最大移动速度（cm/s，来自 Mover 共享设置 CommonLegacyMovementSettings，因角色而异） */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Speed")
    float MaxSpeed = 0.0f;

    /** 速度归一化比率（Speed / MaxSpeed，0..1），供混合空间速度轴/步频表现使用 */
    UPROPERTY(BlueprintReadOnly, Transient, Category = "Locomotion|Speed")
    float SpeedRatio = 0.0f;

    // -------------------------------------------------------------------------
    // 动画资产槽位（父动画蓝图只引用槽位；子动画蓝图 Class Defaults 覆盖为各角色资产）
    // 命名约定：Anim_* = 单段序列；*BlendSpace = 混合空间
    // 可写标记：允许 Lua 在热更新时运行时替换（BlueprintReadWrite，谨慎使用）
    // -------------------------------------------------------------------------

    /** 待机循环动画（Idle 状态） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Slots")
    TObjectPtr<UAnimSequenceBase> Anim_Idle;

    /** 地面移动混合空间（2D：X=前后，Y=左右；输入 = VelocityLocalDir）
     *  注意：UE5.7 起 BlendSpaceBase 已合并进 UBlendSpace（UBlendSpace1D 亦继承自它），槽位统一用 UBlendSpace */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Slots")
    TObjectPtr<UBlendSpace> LocomotionBlendSpace;

    /** 腾空循环动画（Air 状态；起跳/落地等一次性表现走蒙太奇 + FullBody 槽） */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Slots")
    TObjectPtr<UAnimSequenceBase> Anim_AirLoop;

    // -------------------------------------------------------------------------
    // 热更新调参（C++ 仅提供兜底默认值；实际取值由 Lua OnAnimInstanceInitialized 覆盖）
    // -------------------------------------------------------------------------

    /** 进入"移动"状态的速度阈值（cm/s），每角色可在 Lua 中热更调参 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Tuning")
    float MovingSpeedThreshold = 10.0f;

    /** 动画 1.0x 播放速率对应的参考速度（cm/s），用于消除步足滑步；每角色不同 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = "Anim|Tuning")
    float LocomotionReferenceSpeed = 300.0f;

    // -------------------------------------------------------------------------
    // 脚本配置
    // -------------------------------------------------------------------------
    /** Lua 脚本模块路径（相对 Content/Script），子类/子动画蓝图可覆盖换绑专属脚本 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

protected:
    /**
     * 刷新运动数据（子类重写扩展，如多足爬墙角色追加 bIsClimbing）。
     * 注意：本函数在游戏线程（NativeUpdateAnimation 内）执行，
     * 严禁在此调用任何会修改权威世界状态的操作（那些必须放进 MovementMode 模拟回调）。
     */
    virtual void RefreshMovementState(float DeltaSeconds);

    /** 宿主人形/生化角色（缓存自 TryGetPawnOwner） */
    UPROPERTY(Transient)
    TObjectPtr<AGameCharacterBase> OwningCharacter;

    /** 宿主导角色的 Mover 运动组件（速度/模式/落地状态的唯一权威来源） */
    UPROPERTY(Transient)
    TObjectPtr<UCharacterMoverComponent> MoverComponent;

    /** Mover 共享移动设置（取 MaxSpeed 供动画归一化；因角色而异） */
    UPROPERTY(Transient)
    TObjectPtr<UCommonLegacyMovementSettings> MovementSettings;

    /** 上一帧运动模式名（检测模式变化边沿，仅变化时向 Lua 派发表现事件） */
    FName LastMovementModeName;
};
