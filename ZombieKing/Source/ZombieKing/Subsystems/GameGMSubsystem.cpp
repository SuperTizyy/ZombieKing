#include "GameGMSubsystem.h"
#include "GameGameMode.h"             // AGameGameMode（GM 命令派发目标：DispatchGmCommand）
#include "GameTelemetrySubsystem.h"   // 遥测服务（帧时间聚合事件写入其缓冲）
#include "Engine/Engine.h"            // GEngine / FWorldContext（遍历本进程世界上下文，找权威世界）
#include "Engine/GameInstance.h"      // UGameInstance::GetSubsystem（按需取遥测服务）
#include "Engine/World.h"             // UWorld（GetAuthGameMode / GetNetMode / IsGameWorld / FWorldDelegates 声明处）
#include "HAL/IConsoleManager.h"      // FAutoConsoleCommandWithWorldAndArgs（控制台命令注册）
#include "HAL/PlatformTime.h"         // FPlatformTime::Seconds（真实时间：帧时间测量基准）
#include "ZombieKing.h"               // LogZombieKing

// =============================================================================
//  GM 控制台命令注册（进程级、一次成型）
//   * 仅非 Shipping 构建注册：GM 入口是调试工具，发布包不得存在（零作弊面）；
//   * 文件作用域静态 FAutoConsoleCommandWithWorldAndArgs：模块加载即注册、进程存活期一直有效，
//     不依赖任何子系统实例生命周期（处理函数是静态函数，见 GameGMSubsystem.h 注释）；
//   * 命令名固定为单一入口 "GK"：第一个参数是命令 ID、其余是参数串。
//     命令行为全部在 Lua（GameMode.lua 的 GM_COMMANDS 表）——
//     新增/修改命令只热更 Lua、不重编译 C++，这正是"入口在 C++、行为在 Lua"的核心收益。
//   * 静态对象命名带 GS_ 前缀：unity build 下避免与其它翻译单元的同名静态符号冲突（项目规范）。
// =============================================================================
#if !UE_BUILD_SHIPPING
static FAutoConsoleCommandWithWorldAndArgs GS_GMConsoleCommand(
    TEXT("GK"),
    TEXT("GM 调试命令：GK <命令> [参数...]（如 GK.SkipPhase / GK.Infect 玩家名）。"
         "命令行为全部由 Lua 规则脚本实现（GameMode.lua），输入 GK.Help 查看命令列表。仅服务器生效。"),
    FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&UGameGMSubsystem::HandleGmConsoleCommand));
#endif

FName UGameGMSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("GM"));
}

void UGameGMSubsystem::OnServiceInitialize()
{
    // 订阅世界 Tick 事件（帧采样）：
    // 注意与基类纪律一致 —— 此处不得假定"别的服务（如 Telemetry）已就绪"，
    // 遥测服务在"周期到点发事件"时才按需获取（见 EmitFrameTimeSample）。
    TickStartHandle = FWorldDelegates::OnWorldTickStart.AddUObject(this, &UGameGMSubsystem::HandleWorldTickStart);
    TickEndHandle = FWorldDelegates::OnWorldTickEnd.AddUObject(this, &UGameGMSubsystem::HandleWorldTickEnd);
    FrameSamplers.Reset();

    LogService(FString::Printf(TEXT("GM 服务启动（帧采样周期 %.1fs / 卡顿阈值 %.0fms；控制台入口 GK）"),
        FrameSamplePeriodSeconds, FrameSpikeThresholdMs));
}

void UGameGMSubsystem::OnServiceDeinitialize()
{
    // 退订世界 Tick 事件并清空累加器（订阅生命周期对称，防服务销毁后回调悬空）
    FWorldDelegates::OnWorldTickStart.Remove(TickStartHandle);
    FWorldDelegates::OnWorldTickEnd.Remove(TickEndHandle);
    TickStartHandle.Reset();
    TickEndHandle.Reset();
    FrameSamplers.Reset();
}

// =============================================================================
//  GM 控制台命令：解析 → 找权威 GameMode → 转发 Lua
// =============================================================================

void UGameGMSubsystem::HandleGmConsoleCommand(const TArray<FString>& Args, UWorld* World)
{
    // 1) 解析参数：Args[0] = 命令 ID；其余用空格拼成"参数串"（具体切分与语义由 Lua 决定）
    if (Args.Num() <= 0)
    {
        UE_LOG(LogZombieKing, Display, TEXT("[GM] 用法：GK <命令> [参数...]；输入 GK.Help 查看命令列表"));
        return;
    }
    const FString CommandId = Args[0];
    FString ArgsString;
    for (int32 Index = 1; Index < Args.Num(); ++Index)
    {
        if (!ArgsString.IsEmpty())
        {
            ArgsString += TEXT(" ");
        }
        ArgsString += Args[Index];
    }

    // 2) 找到本进程的"权威世界"：
    //    优先用控制台绑定的世界（单机/监听主机在本机控制台输入时即权威世界）；
    //    该世界不存在或非权威（纯客户端控制台）时，退回遍历全部世界上下文找服务器世界。
    //    都找不到 → 拒绝：GM 是服务器侧工具，客户端进程没有改写对局状态的权力。
    UWorld* AuthorityWorld = (World && World->GetAuthGameMode()) ? World : nullptr;
    if (!AuthorityWorld && GEngine)
    {
        for (const FWorldContext& Context : GEngine->GetWorldContexts())
        {
            UWorld* Candidate = Context.World();
            if (Candidate && Candidate->GetAuthGameMode())
            {
                AuthorityWorld = Candidate;
                break;
            }
        }
    }
    if (!AuthorityWorld)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[GM] GK 命令必须在服务器执行（本进程未找到权威世界）"));
        return;
    }

    // 3) 转发给框架 GameMode（其内部再做权威复核）→ OnGmCommand 事件 → Lua 同名函数接管
    AGameGameMode* GameMode = Cast<AGameGameMode>(AuthorityWorld->GetAuthGameMode());
    if (!GameMode)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[GM] 权威世界的 GameMode 不是 AGameGameMode，无法派发 GK 命令"));
        return;
    }
    UE_LOG(LogZombieKing, Display, TEXT("[GM] 控制台命令：GK %s %s（世界=%s）"),
        *CommandId, *ArgsString, *AuthorityWorld->GetName());
    GameMode->DispatchGmCommand(CommandId, ArgsString);
}

// =============================================================================
//  帧时间采样（机制：逐帧真实计时 → 周期聚合 → 写入遥测缓冲；不含任何规则）
// =============================================================================

bool UGameGMSubsystem::ShouldSampleWorld(const UWorld* World, ELevelTick TickType) const
{
    // 开关关闭 → 完全不采样（零开销：不记时间戳、不聚合、不发事件）
    if (!bFrameSamplingEnabled || !World)
    {
        return false;
    }

    // 只采"完整逻辑 Tick"：引擎可能以其它类型（如仅可见性）驱动世界 Tick，
    // 那不是"一帧"的完整成本，计入统计会污染均值
    if (TickType != LEVELTICK_All)
    {
        return false;
    }

    // 只采游戏世界（排除编辑器预览/模板等非对局世界）
    if (!World->IsGameWorld())
    {
        return false;
    }

    // 只采"属于本 GameInstance"的世界：
    //  * PIE：服务器与各客户端世界共享同一个 GameInstance → 全部纳入采样（事件里以世界名区分）；
    //  * 多 GameInstance（多个 PIE 会话）：各实例只采自己的世界 → 不会出现重复上报。
    return World->GetGameInstance() == GetGameInstance();
}

void UGameGMSubsystem::HandleWorldTickStart(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
    if (!ShouldSampleWorld(World, TickType))
    {
        return;
    }

    // 记录本帧起始真实时间戳（FPlatformTime::Seconds，非引擎时间 —— 必须真实时间才测得出真卡顿）
    FGameFrameTimeAccumulator& Accum = FrameSamplers.FindOrAdd(World);
    Accum.TickStartSeconds = FPlatformTime::Seconds();
}

void UGameGMSubsystem::HandleWorldTickEnd(UWorld* World, ELevelTick TickType, float DeltaSeconds)
{
    if (!ShouldSampleWorld(World, TickType))
    {
        return;
    }

    FGameFrameTimeAccumulator* Accum = FrameSamplers.Find(World);
    if (!Accum || Accum->TickStartSeconds <= 0.0)
    {
        // 没有配对的"开始"记录（例如采样中途才开启开关的第一帧）→ 丢弃本帧，不产生假数据
        return;
    }

    const double FrameSeconds = FPlatformTime::Seconds() - Accum->TickStartSeconds;
    Accum->TickStartSeconds = 0.0;
    if (FrameSeconds <= 0.0)
    {
        return;
    }

    // 累计本帧
    Accum->SumFrameSeconds += FrameSeconds;
    Accum->MaxFrameSeconds = FMath::Max(Accum->MaxFrameSeconds, FrameSeconds);
    Accum->FrameCount += 1;
    Accum->PeriodSeconds += FrameSeconds;
    if (FrameSeconds * 1000.0 > FrameSpikeThresholdMs)
    {
        Accum->SpikeCount += 1;
    }

    // 周期攒满 → 发一条遥测事件并复位（周期口径 = 真实帧时间累加，而非墙钟追帧，
    // 保证"世界停 Tick（暂停/后台）"不会被算进周期里稀释统计）
    if (Accum->PeriodSeconds >= FrameSamplePeriodSeconds)
    {
        EmitFrameTimeSample(World, *Accum);
        Accum->ResetStats();
    }
}

void UGameGMSubsystem::EmitFrameTimeSample(const UWorld* World, const FGameFrameTimeAccumulator& Accum)
{
    // 写入既有遥测服务缓冲（不造新轮子）：上报、聚合、查询全部复用 Telemetry 链路。
    // 按需获取（不缓存指针）：OnServiceInitialize 时其它服务可能尚未创建（引擎顺序不保证）。
    const UGameInstance* GameInstance = GetGameInstance();
    UGameTelemetrySubsystem* Telemetry = GameInstance ? GameInstance->GetSubsystem<UGameTelemetrySubsystem>() : nullptr;
    if (!Telemetry || !Telemetry->IsTelemetryEnabled())
    {
        return;
    }

    const double MeanMs = Accum.FrameCount > 0 ? (Accum.SumFrameSeconds / Accum.FrameCount) * 1000.0 : 0.0;
    const bool bAuthority = (World->GetAuthGameMode() != nullptr);

    FGameTelemetryEvent Event;
    Event.EventName = FName(TEXT("Perf.FrameTime"));
    // ServerTimeSeconds 留空：遥测服务自动补当前世界时间
    Event.PayloadJson = FString::Printf(
        TEXT("{\"world\":\"%s\",\"authority\":%s,\"netMode\":%d,\"periodSeconds\":%.2f,"
             "\"frames\":%d,\"meanMs\":%.2f,\"maxMs\":%.2f,\"spikes\":%d,\"spikeThresholdMs\":%.1f}"),
        *World->GetName(),
        bAuthority ? TEXT("true") : TEXT("false"),
        static_cast<int32>(World->GetNetMode()),
        Accum.PeriodSeconds,
        Accum.FrameCount,
        MeanMs,
        Accum.MaxFrameSeconds * 1000.0,
        Accum.SpikeCount,
        FrameSpikeThresholdMs);

    Telemetry->RecordEvent(MoveTemp(Event));
}
