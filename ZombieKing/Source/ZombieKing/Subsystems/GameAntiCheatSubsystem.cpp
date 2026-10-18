#include "GameAntiCheatSubsystem.h"
#include "Engine/World.h"   // UWorld::GetTimeSeconds（给证据打服务器时间戳）
#include "ZombieKing.h"     // LogZombieKing（反作弊取证日志）

void UGameAntiCheatSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    // 先调用 Super：引擎基类内部有世界级子系统的登记流程
    Super::Initialize(Collection);

    // 世界创建即复位取证状态：保证每个世界（每场对局）从干净记录开始
    Reports.Reset();
    FlaggedPlayerIds.Reset();

    UE_LOG(LogZombieKing, Log, TEXT("[反作弊] 世界级反作弊服务已启动"));
}

void UGameAntiCheatSubsystem::Deinitialize()
{
    // 世界销毁：清空记录与标记（内存态数据，无需外部资源释放）
    Reports.Reset();
    FlaggedPlayerIds.Reset();

    UE_LOG(LogZombieKing, Log, TEXT("[反作弊] 世界级反作弊服务已关闭"));

    // 最后调用 Super：由引擎基类完成注销等收尾
    Super::Deinitialize();
}

void UGameAntiCheatSubsystem::ReportCheat(int32 PlayerId, EGameCheatCategory Category, const FString& Details)
{
    // 边界校验：无效 PlayerId 直接拒绝，避免污染归档与标记集合
    if (PlayerId == INDEX_NONE)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[反作弊] 上报被拒绝：PlayerId 无效"));
        return;
    }

    // 组装证据记录：只存稳定标识与文本细节，不持有任何对象指针（避免阻止 GC）
    FGameCheatReport Report;
    Report.PlayerId = PlayerId;
    Report.Category = Category;
    Report.Details = Details;

    // 打服务器时间戳：用于与对局录像/回放对齐（GetWorld() 由 UWorldSubsystem 提供，且仅在本世界有效）
    if (const UWorld* World = GetWorld())
    {
        Report.ServerTimeSeconds = World->GetTimeSeconds();
    }

    // 归档（本世界的全量证据链，供事后审计与申诉）
    Reports.Add(Report);

    // 标记该玩家：后续查询 O(1)；注意这里只是"标记"，不是"处置"（处置规则见类注释）
    FlaggedPlayerIds.Add(PlayerId);

    // 取证日志：线上排障最关心"谁、哪一类、什么时候"，故按固定格式输出，便于日志检索
    UE_LOG(LogZombieKing, Warning, TEXT("[反作弊] 玩家 %d 触发可疑行为（类别索引 %d）：%s"),
        PlayerId, static_cast<int32>(Category), *Details);

    // 广播：Lua 规则脚本 / 后端上报模块 / 管理员工具订阅
    OnCheatReported.Broadcast(Report);

    // TODO（后续里程碑）：把证据实时上报服务端安全模块（当前只本地归档 + 广播）。
}

bool UGameAntiCheatSubsystem::IsPlayerFlagged(int32 PlayerId) const
{
    // 直接查标记集合：O(1)，可被每帧/高频的表现层安全调用
    return FlaggedPlayerIds.Contains(PlayerId);
}
