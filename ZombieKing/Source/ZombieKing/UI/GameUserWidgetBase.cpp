#include "GameUserWidgetBase.h"
#include "Subsystems/GameUISubsystem.h"   // UGameUISubsystem（界面脚本取用 UI 管理层）
#include "Engine/World.h"                 // GetWorld()->GetGameInstance()
#include "Engine/GameInstance.h"          // UGameInstance::GetSubsystem
#include "ZombieKing.h"                   // LogZombieKing

UGameUserWidgetBase::UGameUserWidgetBase(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // 绑定前端界面的默认共享脚本（可由各 WBP 覆盖为自己的脚本模块）
    LuaScriptPath = TEXT("Lua.Core.UI.GameUserWidgetBase");
}

FString UGameUserWidgetBase::GetModuleName_Implementation() const
{
    // 优先用界面自己配置的脚本；未配置则回落默认共享脚本（仅打日志，保证界面可用且不报错）。
    // 注意：默认模块名必须写成函数内字面量 —— UBT unity build 下，匿名命名空间的同名全局常量会跨文件重定义。
    return LuaScriptPath.IsEmpty()
        ? FString(TEXT("Lua.Core.UI.GameUserWidgetBase"))
        : LuaScriptPath;
}

void UGameUserWidgetBase::SetRouteInfo(FName InRoute, EGameUILayer InLayer)
{
    // 路由与层级由 UI 管理层在创建界面后立即写入，供界面脚本查询"我是谁、在哪一层"
    Route = InRoute;
    Layer = InLayer;
}

UGameUISubsystem* UGameUserWidgetBase::GetUISubsystem() const
{
    // 经世界取 GameInstance，再取其上的 UI 管理层服务（界面可能在世界切换期间被创建，故每处判空）
    const UWorld* World = GetWorld();
    if (!World)
    {
        return nullptr;
    }

    UGameInstance* GameInstance = World->GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameUISubsystem>() : nullptr;
}

void UGameUserWidgetBase::NativeConstruct()
{
    // 先让 Super 完成 UMG 自身的构造流程（绑定动画、缓动、脚本等），再派发 Lua 事件
    Super::NativeConstruct();

    // 派发 Lua 显示事件：界面脚本在此初始化显示、拉取数据、播放入场动画
    OnWidgetShow();
}

void UGameUserWidgetBase::NativeDestruct()
{
    // 先派发 Lua 隐藏事件：此刻控件仍然有效，Lua 能安全地停止动画、解绑委托
    OnWidgetHide();

    // 再走 Super 的析构流程（此后控件逐步失效）
    Super::NativeDestruct();
}
