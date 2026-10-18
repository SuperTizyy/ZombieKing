#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineBaseTypes.h"   // ELevelTick（FWorldDelegates 世界 Tick 事件的形参类型）
#include "GameServiceSubsystem.h"     // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameGMSubsystem.generated.h"

class UWorld;

/**
 * 帧时间采样累加器（每个世界一份；纯 C++ 统计，不含任何规则）。
 *
 * 口径说明：帧时间用"世界 Tick 开始 → Tick 结束"的**真实时间**（FPlatformTime::Seconds）测量，
 *           而不是引擎传入的 DeltaTime —— DeltaTime 被引擎钳制过（会隐藏超长帧），
 *           而"卡顿尖峰"正是竞技游戏性能分析的核心指标，必须用未钳制的真实墙钟时间才测得到。
 */
struct FGameFrameTimeAccumulator
{
    /** 本帧 Tick 开始的真实时间戳（秒）；0 = 尚未记录（本帧不计） */
    double TickStartSeconds = 0.0;
    /** 当前统计周期内累计的真实帧时间（秒）；达到周期 → 发一条遥测事件并复位 */
    double PeriodSeconds = 0.0;
    /** 周期内帧时间总和（秒；用于求均值） */
    double SumFrameSeconds = 0.0;
    /** 周期内最长单帧（秒；最坏卡顿） */
    double MaxFrameSeconds = 0.0;
    /** 周期内帧数 */
    int32 FrameCount = 0;
    /** 周期内卡顿尖峰数（真实帧时间超过阈值的帧） */
    int32 SpikeCount = 0;

    /** 复位统计量（周期发事件后调用；TickStartSeconds 不在此复位，由采样流程自清） */
    void ResetStats()
    {
        PeriodSeconds = 0.0;
        SumFrameSeconds = 0.0;
        MaxFrameSeconds = 0.0;
        FrameCount = 0;
        SpikeCount = 0;
    }
};

/**
 * 竞技生化网游：GM 调试 + 性能观测服务（GameInstance 层服务，服务名 "GM"）
 *
 * 职责（两件"服务器侧观测/调试"能力，刻意收敛在同一个服务里）：
 *  1) GM 控制台命令：注册全局控制台命令 "GK <命令> [参数...]"（仅非 Shipping 构建存在该入口），
 *     本服务只做"解析 + 找到本进程权威 GameMode + 转发"，**命令行为全部下沉 Lua**
 *     （GameMode.lua 的 GM_COMMANDS 表）—— 新增/修改命令只热更 Lua、不重编译 C++；
 *  2) 帧时间采样埋点：订阅 FWorldDelegates 的 Tick 开始/结束事件，按周期聚合
 *     "均值帧时间 / 最长帧 / 卡顿尖峰计数"，写入既有遥测服务（UGameTelemetrySubsystem）的缓冲，
 *     不另造上报轮子；事件名 "Perf.FrameTime"，载荷为 JSON（含世界名/是否权威/NetMode 等）。
 *
 * 为什么 GM 入口做成"控制台命令 + 行为下沉 Lua"（用户拍板的架构）：
 *  - 控制台是引擎原生、零 UI 成本的调试入口，开发期随开随用；
 *  - 命令语义全是"规则级动作"（跳阶段/感染/改等级），本就该住在 Lua 规则层；
 *  - C++ 只留"注册/解析/派发"这层薄机制，命令集演进不再触碰编译。
 *
 * 安全边界：
 *  - Shipping 构建不注册 GK 命令（发布包零作弊面）；
 *  - 派发只打到"权威 GameMode"（客户端进程找不到权威世界即拒绝）；
 *  - GameMode.DispatchGmCommand 内部再做一次 HasAuthority 复核（纵深防御）。
 */
UCLASS()
class ZOMBIEKING_API UGameGMSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 帧采样开关（BlueprintCallable：Lua/配置可热更控制，默认开启）
    // -------------------------------------------------------------------------
    /** 开启/关闭帧时间采样（重度调试时可关掉降噪，零开销：不记时间戳、不聚合） */
    UFUNCTION(BlueprintCallable, Category = "GM|Perf")
    void SetFrameSamplingEnabled(bool bEnabled) { bFrameSamplingEnabled = bEnabled; }

    /** 当前是否开启帧时间采样 */
    UFUNCTION(BlueprintPure, Category = "GM|Perf")
    bool IsFrameSamplingEnabled() const { return bFrameSamplingEnabled; }

    // -------------------------------------------------------------------------
    // GM 控制台命令（入口："GK <命令> [参数...]"）
    // -------------------------------------------------------------------------
    /**
     * 控制台命令处理（静态函数；供文件作用域的 FAutoConsoleCommandWithWorldAndArgs 绑定，故必须 public）。
     * 为什么是静态：控制台命令对象是"进程级全局"（注册一次、全进程共享）；
     * 若绑定到子系统实例，PIE 多 GameInstance 会触发"重复注册"冲突，
     * 且实例可能先于命令销毁导致回调悬空。静态入口"按需解析当前权威 GameMode"，
     * 不依赖任何实例生命周期。
     * @param Args  命令行参数（Args[0]=命令 ID；其余为参数；引擎委托约定参数顺序为"参数在前"）
     * @param World 打开控制台的客户端绑定世界（可能为空或非权威世界，内部自行兜底查找）
     */
    static void HandleGmConsoleCommand(const TArray<FString>& Args, UWorld* World);

protected:
    /** 启动：订阅世界 Tick 事件（帧采样） */
    virtual void OnServiceInitialize() override;

    /** 关闭：退订世界 Tick 事件并清空累加器 */
    virtual void OnServiceDeinitialize() override;

private:
    // -------------------------------------------------------------------------
    // 帧时间采样（订阅 FWorldDelegates 世界 Tick 事件）
    // -------------------------------------------------------------------------
    /** 世界 Tick 开始：记录本帧真实起始时间 */
    void HandleWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds);

    /** 世界 Tick 结束：累计本帧真实耗时；周期到点 → 发遥测事件并复位 */
    void HandleWorldTickEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds);

    /** 该世界是否纳入采样：本 GameInstance 所属 + 游戏世界 + 完整逻辑 Tick */
    bool ShouldSampleWorld(const UWorld* World, ELevelTick TickType) const;

    /** 周期到点：把"帧时间聚合"写成一条遥测事件（Perf.FrameTime）交给 Telemetry 服务缓冲 */
    void EmitFrameTimeSample(const UWorld* World, const FGameFrameTimeAccumulator& Accum);

    /** 世界 Tick 事件订阅句柄（服务关闭时据此退订，保持订阅生命周期对称） */
    FDelegateHandle TickStartHandle;
    FDelegateHandle TickEndHandle;

    /** 逐世界帧时间累加器（弱引用键：世界销毁不阻碍 GC，陈旧条目自然失效） */
    TMap<TWeakObjectPtr<UWorld>, FGameFrameTimeAccumulator> FrameSamplers;

    /** 帧采样是否启用（默认开；SetFrameSamplingEnabled 可关） */
    bool bFrameSamplingEnabled = true;

    /** 统计周期（秒）：每攒满一个周期的真实帧时间，发一条 "Perf.FrameTime" 遥测事件 */
    UPROPERTY(EditDefaultsOnly, Category = "GM|Perf")
    double FrameSamplePeriodSeconds = 5.0;

    /** 卡顿尖峰阈值（毫秒）：真实帧时间超过该值即计一次尖峰（竞技 60fps 基准：50ms ≈ 掉到 20fps 以下） */
    UPROPERTY(EditDefaultsOnly, Category = "GM|Perf")
    double FrameSpikeThresholdMs = 50.0;
};
