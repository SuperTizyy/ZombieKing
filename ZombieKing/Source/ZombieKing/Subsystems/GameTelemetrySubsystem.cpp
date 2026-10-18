#include "GameTelemetrySubsystem.h"
#include "Engine/GameInstance.h"   // UGameInstance::GetWorld()（取当前世界以填写服务器时间）
#include "Engine/World.h"          // UWorld::GetTimeSeconds（服务器世界时间）
#include "ZombieKing.h"            // LogZombieKing（遥测落库/丢弃日志）

FName UGameTelemetrySubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Telemetry"));
}

void UGameTelemetrySubsystem::OnServiceInitialize()
{
    // 启动即复位：缓冲清空、开关回到默认启用，保证每次运行从干净状态开始
    BufferedEvents.Reset();
    bTelemetryEnabled = true;
}

void UGameTelemetrySubsystem::OnServiceDeinitialize()
{
    // 关闭时清空缓冲。
    // TODO（接入正式后端）：应在此处对未上报事件做一次同步冲刷（Shutdown 阶段网络可能已断，
    //   故正式实现更可能是"写入本地落盘缓存，下次启动补报"）。
    BufferedEvents.Reset();
}

void UGameTelemetrySubsystem::RecordEvent(FGameTelemetryEvent Event)
{
    // 未启用则直接丢弃：保证关闭遥测后零开销（不再补时间戳、不再入缓冲、不再广播）
    if (!bTelemetryEnabled)
    {
        return;
    }

    // 调用方未填写时间时，自动补"服务器世界时间"：保证多条事件可按时间排序与回放
    if (Event.ServerTimeSeconds <= 0.0)
    {
        // GameInstanceSubsystem 无 GetWorld()，需经 GameInstance 取当前世界
        if (const UGameInstance* GameInstance = GetGameInstance())
        {
            if (const UWorld* World = GameInstance->GetWorld())
            {
                Event.ServerTimeSeconds = World->GetTimeSeconds();
            }
        }
    }

    // 写入缓冲（按值移入，避免多余拷贝）
    BufferedEvents.Add(MoveTemp(Event));

    // 容量保护：超出上限则丢弃最旧一条（远早于上报窗口，丢失代价最小），防止内存无界增长
    if (MaxBufferedEvents > 0 && BufferedEvents.Num() > MaxBufferedEvents)
    {
        BufferedEvents.RemoveAt(0);
    }

    // 广播给订阅方（如后端上报模块、Lua 表现层）
    OnTelemetryEvent.Broadcast(BufferedEvents.Last());

    // TODO（后续里程碑）：接入正式后端上报（批量、异步、失败重试、限流）；
    //   当前仅做本地缓冲 + 广播，避免在没有后端时引入无效的网络依赖。
}

void UGameTelemetrySubsystem::ClearBufferedEvents()
{
    // 上报成功（或主动丢弃）后清空，避免缓冲重复积压
    BufferedEvents.Reset();
}
