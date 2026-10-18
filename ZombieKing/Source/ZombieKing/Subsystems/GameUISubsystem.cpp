#include "GameUISubsystem.h"
#include "GameUserWidgetBase.h"      // 界面基类（本服务创建/持有的控件类型）
#include "GameUIRouteTable.h"        // 路由表（路由 → 界面类 + 层级）
#include "GameUISettings.h"          // 界面配置（路由表软引用、是否自动切输入模式）
#include "Blueprint/UserWidget.h"    // CreateWidget
#include "GameFramework/PlayerController.h" // SetInputMode / bShowMouseCursor / FInputModeUIOnly 等
#include "Engine/GameInstance.h"     // GetFirstLocalPlayerController
#include "Engine/World.h"            // GetWorld
#include "ZombieKing.h"              // LogZombieKing

FName UGameUISubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("UI"));
}

void UGameUISubsystem::OnServiceInitialize()
{
    // 启动即复位：清空界面栈与路由表缓存（GameInstance 在编辑器内可能被复用）
    ActiveWidgets.Reset();
    RouteTable = nullptr;
}

void UGameUISubsystem::OnServiceDeinitialize()
{
    // 关闭时把所有界面移出视口，避免关卡切换/退出编辑器时控件残留在视口里。
    // 这里刻意不广播弹出事件：服务已进入关闭流程，外部系统不应再据此做业务响应。
    for (int32 Index = ActiveWidgets.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveWidgets[Index].Widget)
        {
            ActiveWidgets[Index].Widget->RemoveFromParent();
        }
    }
    ActiveWidgets.Reset();
    RouteTable = nullptr;
}

UGameUIRouteTable* UGameUISubsystem::GetRouteTable()
{
    // 已加载过则直接复用（界面推入是高频操作，不能每次都去碰资产加载）
    if (RouteTable)
    {
        return RouteTable;
    }

    const UGameUISettings* Settings = GetDefault<UGameUISettings>();
    if (Settings->RouteTable.IsNull())
    {
        UE_LOG(LogZombieKing, Error,
            TEXT("[界面] 未配置路由表：请在 Project Settings → Game → ZombieKing 界面 里指定 UGameUIRouteTable 资产。"));
        return nullptr;
    }

    // 同步加载路由表（它是 UI 的入口资产，必须即时可用；未加载成功则推入界面无从查表）
    RouteTable = Settings->RouteTable.LoadSynchronous();
    if (!RouteTable)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[界面] 路由表加载失败：%s"), *Settings->RouteTable.ToString());
    }
    return RouteTable;
}

APlayerController* UGameUISubsystem::GetOwningPlayerController() const
{
    // UI 只属于"本机玩家"：专用服务器 / 无本地视口时返回 null（调用方据此拒绝推入界面）
    const UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetFirstLocalPlayerController() : nullptr;
}

int32 UGameUISubsystem::CountWidgetsOnInputLayer() const
{
    int32 Count = 0;
    for (const FGameUIActiveWidget& Record : ActiveWidgets)
    {
        if (GameUILayerLayout::DoesLayerCaptureInput(Record.Layer))
        {
            ++Count;
        }
    }
    return Count;
}

int32 UGameUISubsystem::GetZOrderForPush(EGameUILayer Layer) const
{
    // ZOrder = 层级基准 + 该层已有数量：保证层级之间有固定区间，层内后推入者在上。
    // 注意：必须在"记录新界面之前"调用，否则计数会把本次自己算进去。
    int32 SameLayerCount = 0;
    for (const FGameUIActiveWidget& Record : ActiveWidgets)
    {
        if (Record.Layer == Layer)
        {
            ++SameLayerCount;
        }
    }
    return GameUILayerLayout::GetZOrderBase(Layer) + SameLayerCount;
}

bool UGameUISubsystem::PushRoute(FName Route)
{
    // 路由必须有效
    if (Route.IsNone())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[界面] 推入被拒绝：路由名为空"));
        return false;
    }

    // 只有本机玩家才有界面：专用服务器与"尚未创建本地控制器"的时机都应安全拒绝
    APlayerController* OwningPlayerController = GetOwningPlayerController();
    if (!OwningPlayerController)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[界面] 推入被拒绝（%s）：当前没有本机玩家控制器"), *Route.ToString());
        return false;
    }

    // 同名界面不允许同时存在两个：避免"同一界面被推两次"导致的输入与生命周期混乱
    if (IsRouteActive(Route))
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[界面] 推入被拒绝（%s）：该路由已存在"), *Route.ToString());
        return false;
    }

    UGameUIRouteTable* Table = GetRouteTable();
    if (!Table)
    {
        return false;
    }

    // 查表拿"界面类 + 层级"：UI 管理层不认识任何具体界面，全靠路由表解耦
    FGameUIRouteEntry Entry;
    if (!Table->FindRoute(Route, Entry))
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[界面] 推入被拒绝（%s）：路由表中没有该条目"), *Route.ToString());
        return false;
    }

    // 按需加载界面类（软类引用：不用的界面不会被拖进内存）
    UClass* WidgetClass = Entry.WidgetClass.LoadSynchronous();
    if (!WidgetClass)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[界面] 推入失败（%s）：界面类未配置或加载失败（%s）"),
            *Route.ToString(), *Entry.WidgetClass.ToString());
        return false;
    }

    // 创建控件：Owner 传本机控制器，使界面内 GetOwningPlayer() 能拿回控制器（进而取会话/流程数据）
    UGameUserWidgetBase* Widget = CreateWidget<UGameUserWidgetBase>(OwningPlayerController, WidgetClass);
    if (!Widget)
    {
        UE_LOG(LogZombieKing, Error, TEXT("[界面] 推入失败（%s）：控件创建失败"), *Route.ToString());
        return false;
    }

    // 先算 ZOrder（必须在本界面被记入活动列表之前），再写入路由信息
    const int32 ZOrder = GetZOrderForPush(Entry.Layer);
    Widget->SetRouteInfo(Route, Entry.Layer);

    // 只有"接收输入的层"才参与命中测试；背景层/覆盖提示层设为 SelfHitTestInvisible，
    // 避免它们挡住对局操作（FPS 里尤其致命：点一下鼠标却被 UI 吃掉了）。
    Widget->SetVisibility(GameUILayerLayout::DoesLayerCaptureInput(Entry.Layer)
        ? ESlateVisibility::Visible
        : ESlateVisibility::SelfHitTestInvisible);

    // 先登记再加入视口：AddToViewport 会触发 NativeConstruct（进而派发 Lua 的 OnWidgetShow），
    // 若 Lua 在该回调里又尝试推同名路由，登记在先能让它被"IsRouteActive"正确拒绝（避免重复推入）。
    FGameUIActiveWidget& Record = ActiveWidgets.AddDefaulted_GetRef();
    Record.Layer = Entry.Layer;
    Record.Route = Route;
    Record.Widget = Widget;

    Widget->AddToViewport(ZOrder);

    // 有交互界面在场就切 UI 输入模式并放开鼠标；否则回到游戏输入模式
    ApplyInputMode();

    UE_LOG(LogZombieKing, Log, TEXT("[界面] 已推入 %s（层级 %d，ZOrder %d）"),
        *Route.ToString(), static_cast<int32>(Entry.Layer), ZOrder);

    OnRoutePushed.Broadcast(Route, Entry.Layer, Widget);
    return true;
}

bool UGameUISubsystem::PopRouteOnLayer(EGameUILayer Layer)
{
    // 栈语义：从后往前找该层级最新推入的一个（即该层的栈顶）
    for (int32 Index = ActiveWidgets.Num() - 1; Index >= 0; --Index)
    {
        if (ActiveWidgets[Index].Layer == Layer)
        {
            RemoveActiveWidgetAt(Index, true);
            return true;
        }
    }

    UE_LOG(LogZombieKing, Warning, TEXT("[界面] 弹出被忽略：层级 %d 上没有活动界面"), static_cast<int32>(Layer));
    return false;
}

bool UGameUISubsystem::CloseRoute(FName Route)
{
    for (int32 Index = 0; Index < ActiveWidgets.Num(); ++Index)
    {
        if (ActiveWidgets[Index].Route == Route)
        {
            RemoveActiveWidgetAt(Index, true);
            return true;
        }
    }
    return false;
}

void UGameUISubsystem::CloseAllRoutes()
{
    // 自栈顶向下逐个关闭：与"手动逐个关闭"顺序一致，Lua 收到的事件顺序也符合直觉。
    // 用下标递减遍历：删除靠后的元素不会影响前面的下标；期间若 Lua 又推入新界面，
    // 它会追加到数组末尾而不会被本次循环清掉（属预期行为：新推入的界面应保留）。
    for (int32 Index = ActiveWidgets.Num() - 1; Index >= 0; --Index)
    {
        RemoveActiveWidgetAt(Index, true);
    }
}

UGameUserWidgetBase* UGameUISubsystem::FindWidget(FName Route) const
{
    for (const FGameUIActiveWidget& Record : ActiveWidgets)
    {
        if (Record.Route == Route)
        {
            return Record.Widget;
        }
    }
    return nullptr;
}

bool UGameUISubsystem::IsRouteActive(FName Route) const
{
    return FindWidget(Route) != nullptr;
}

void UGameUISubsystem::RemoveActiveWidgetAt(int32 Index, bool bBroadcast)
{
    // 越界防御：本函数是唯一的内部移除入口，边界必须自证安全
    if (!ActiveWidgets.IsValidIndex(Index))
    {
        return;
    }

    // 先复制一份记录：RemoveFromParent 会触发 Lua 的 OnWidgetHide，那段脚本可能重入本服务改动列表
    const FGameUIActiveWidget Record = ActiveWidgets[Index];

    ActiveWidgets.RemoveAt(Index);

    // 移出视口：其引用仍被局部变量 Record 持有，故回调期间控件一定有效
    if (Record.Widget)
    {
        Record.Widget->RemoveFromParent();
    }

    // 输入模式随"交互界面数量"变化
    ApplyInputMode();

    if (bBroadcast)
    {
        UE_LOG(LogZombieKing, Log, TEXT("[界面] 已弹出 %s（层级 %d）"),
            *Record.Route.ToString(), static_cast<int32>(Record.Layer));
        OnRoutePopped.Broadcast(Record.Route, Record.Layer, Record.Widget);
    }
}

void UGameUISubsystem::ApplyInputMode()
{
    // 配置允许关闭自动切换（如纯服务器环境）
    const UGameUISettings* Settings = GetDefault<UGameUISettings>();
    if (!Settings->bAutoApplyInputMode)
    {
        return;
    }

    APlayerController* OwningPlayerController = GetOwningPlayerController();
    if (!OwningPlayerController)
    {
        return;
    }

    if (CountWidgetsOnInputLayer() > 0)
    {
        // 有交互界面在场：切 UI 输入模式并显示鼠标（菜单需要点选），
        // 否则鼠标会被锁定在视口内、菜单根本点不到。
        FInputModeUIOnly InputMode;
        OwningPlayerController->SetInputMode(InputMode);
        OwningPlayerController->bShowMouseCursor = true;
    }
    else
    {
        // 没有交互界面（例如对局中只剩 HUD/提示）：回到游戏输入模式并隐藏鼠标。
        // 竞技 FPS 必须如此，否则鼠标光标会一直悬在屏幕上干扰瞄准。
        FInputModeGameOnly InputMode;
        OwningPlayerController->SetInputMode(InputMode);
        OwningPlayerController->bShowMouseCursor = false;
    }
}
