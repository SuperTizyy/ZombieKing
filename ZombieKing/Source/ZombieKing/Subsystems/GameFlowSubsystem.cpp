#include "GameFlowSubsystem.h"
#include "ZombieKing.h"   // LogZombieKing

FName UGameFlowSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Flow"));
}

void UGameFlowSubsystem::OnServiceInitialize()
{
    // 进程启动必然处于"启动"阶段：复位到起点，保证流程状态不残留（编辑器内 GameInstance 复用场景）
    FlowStage = EGameFlowStage::Boot;
}

void UGameFlowSubsystem::SetFlowStage(EGameFlowStage NewStage)
{
    // 幂等：阶段未变化则不广播，避免订阅方收到重复事件
    if (FlowStage == NewStage)
    {
        return;
    }

    // 先记录旧阶段再迁移：广播需要同时给出"新"与"旧"，便于表现层播放转场/做差异处理
    const EGameFlowStage OldStage = FlowStage;
    FlowStage = NewStage;

    UE_LOG(LogZombieKing, Log, TEXT("[流程] 阶段迁移：%d → %d"),
        static_cast<int32>(OldStage), static_cast<int32>(NewStage));

    // 广播：UI 脚本 / 音效 / 遥测等订阅（C++ 侧不据此做规则判断）
    OnFlowStageChanged.Broadcast(NewStage, OldStage);
}
