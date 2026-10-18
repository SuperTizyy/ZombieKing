#include "GameServiceSubsystem.h"
#include "ZombieKing.h"   // LogZombieKing（本服务的统一日志分类）

void UGameServiceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    // 先调用 Super：引擎基类内部要完成子系统登记等基础工作，跳过它会导致引擎流程不完整
    Super::Initialize(Collection);

    // 置位就绪标志后再调用子类钩子：保证子类在钩子内查询 IsServiceReady() 时结果与"已启动"一致
    bServiceReady = true;

    // 转发到子类实现点：资源申请 / 委托注册只写在这里
    OnServiceInitialize();

    // 统一收尾日志（带服务短名前缀），便于确认服务已进入可用状态
    LogService(TEXT("服务已启动"));
}

void UGameServiceSubsystem::Deinitialize()
{
    // 先摘掉就绪标志：避免关闭过程中外部仍按"服务可用"调用进来，造成半关闭状态的错用
    bServiceReady = false;

    // 转发到子类实现点：资源释放 / 委托解绑只写在这里
    OnServiceDeinitialize();

    // 统一收尾日志
    LogService(TEXT("服务已关闭"));

    // 最后调用 Super：让引擎基类完成注销等收尾工作
    Super::Deinitialize();
}

void UGameServiceSubsystem::LogService(const FString& Message) const
{
    // 统一前缀 "[服务:<短名>]"，便于在 LogZombieKing 输出中按服务过滤与排障
    UE_LOG(LogZombieKing, Log, TEXT("[服务:%s] %s"), *GetServiceName().ToString(), *Message);
}
