#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"   // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameUITypes.h"            // EGameUILayer（界面层级）
#include "GameUISubsystem.generated.h"

class UGameUserWidgetBase;          // 前端界面基类（本服务创建/持有/销毁的界面实例类型）
class UGameUIRouteTable;            // 界面路由表数据资产（路由 → 界面类 + 层级）
class APlayerController;

/** 界面路由变更广播（推入 / 弹出各一份；供 UI 脚本、流程服务、音效等订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnGameUIRouteChanged, FName, Route, EGameUILayer, Layer, UGameUserWidgetBase*, Widget);

/**
 * 活动界面记录（层级 + 路由 + 控件实例）。
 * 用 UPROPERTY 持有 → 被 GC 追踪，避免"界面已从视口移除但引用丢失"导致的悬空。
 */
USTRUCT()
struct FGameUIActiveWidget
{
    GENERATED_BODY()

    /** 所在层级 */
    UPROPERTY()
    EGameUILayer Layer = EGameUILayer::Window;

    /** 路由名 */
    UPROPERTY()
    FName Route;

    /** 控件实例 */
    UPROPERTY()
    TObjectPtr<UGameUserWidgetBase> Widget;
};

/**
 * 竞技生化网游：UI 管理层服务（GameInstance 层服务）
 *
 * 定位：**所有界面的唯一出入口**。界面不允许自行 `AddToViewport` —— 必须经本服务推入，
 *       这样才能保证：层级 ZOrder 永远有序、输入模式与鼠标显隐始终正确、界面生命周期统一收口。
 *
 * 职责边界（严格遵循"机制在 C++、表现与规则在 Lua"）：
 *  - 本服务只做**机制**：按路由查表 → 创建界面 → 分层入栈 → 输入模式推导 → 生命周期事件派发 → 统一销毁；
 *  - "有哪些界面、路由叫什么、放在哪一层"是**内容**（UGameUIRouteTable 数据资产 + UGameUISettings 配置）；
 *  - "什么条件弹什么界面"是**规则**，写在 Lua 脚本里（界面脚本 / 流程脚本），改规则不需重编译。
 *
 * 生命周期：随 GameInstance 存活（跨关卡不重建），因此换图时界面栈不需要重建（由 Lua 规则决定何时清栈）。
 *
 * 与前端流程服务（UGameFlowSubsystem）的协作：本服务不认识"流程阶段"，
 * 只负责把界面按路由放对位置；"进入房间该弹哪个界面"由流程规则（Lua）调用本服务实现。
 */
UCLASS()
class ZOMBIEKING_API UGameUISubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 路由栈操作（BlueprintCallable：供 Lua 规则脚本驱动）
    // -------------------------------------------------------------------------
    /**
     * 按路由推入一个界面（要求本机存在 LocalPlayerController；专用服务器不会有界面）。
     * 内部完成：查路由表 → 加载界面类 → 创建控件 → 按层级分配 ZOrder → 加入视口 → 切换输入模式。
     * @param Route 路由名（须在路由表中有条目）
     * @return 是否成功推入（false = 无本机玩家 / 路由未登记 / 该类加载失败 / 同名路由已存在）
     */
    UFUNCTION(BlueprintCallable, Category = "UI")
    bool PushRoute(FName Route);

    /**
     * 弹出指定层级**最新推入**的界面（栈语义：先进后出）。
     * @param Layer 目标层级
     * @return 是否弹出了界面
     */
    UFUNCTION(BlueprintCallable, Category = "UI")
    bool PopRouteOnLayer(EGameUILayer Layer);

    /**
     * 关闭指定路由的界面（不论它在第几层）。
     * @param Route 目标路由名
     * @return 是否找到并关闭
     */
    UFUNCTION(BlueprintCallable, Category = "UI")
    bool CloseRoute(FName Route);

    /** 关闭全部界面（如：切换关卡/退出对局前清栈） */
    UFUNCTION(BlueprintCallable, Category = "UI")
    void CloseAllRoutes();

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    /** 按路由取当前存活的界面实例（不存在时为 null） */
    UFUNCTION(BlueprintPure, Category = "UI")
    UGameUserWidgetBase* FindWidget(FName Route) const;

    /** 该路由当前是否已推入 */
    UFUNCTION(BlueprintPure, Category = "UI")
    bool IsRouteActive(FName Route) const;

    /** 当前活动界面总数 */
    UFUNCTION(BlueprintPure, Category = "UI")
    int32 GetActiveWidgetCount() const { return ActiveWidgets.Num(); }

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：Lua / 流程服务 / 音效订阅）
    // -------------------------------------------------------------------------
    /** 界面被推入（加入视口后广播） */
    UPROPERTY(BlueprintAssignable, Category = "UI")
    FOnGameUIRouteChanged OnRoutePushed;

    /** 界面被弹出（移出视口后广播） */
    UPROPERTY(BlueprintAssignable, Category = "UI")
    FOnGameUIRouteChanged OnRoutePopped;

protected:
    /** 启动：清空界面栈与路由表缓存 */
    virtual void OnServiceInitialize() override;

    /** 关闭：把所有界面移出视口，避免关卡切换/退出时控件残留 */
    virtual void OnServiceDeinitialize() override;

private:
    /** 惰性加载路由表（首次推入界面时加载；未配置或加载失败返回 null 并告警） */
    UGameUIRouteTable* GetRouteTable();

    /** 取本机玩家控制器（专用服务器 / 无本地视口时返回 null） */
    APlayerController* GetOwningPlayerController() const;

    /** 移除指定下标的活动界面（bBroadcast 决定是否广播弹出事件） */
    void RemoveActiveWidgetAt(int32 Index, bool bBroadcast);

    /** 按"当前是否有交互界面"推导并应用输入模式与鼠标显隐 */
    void ApplyInputMode();

    /** 统计当前处于"会接收输入的层级"的界面数量 */
    int32 CountWidgetsOnInputLayer() const;

    /** 计算某层级本次推入应使用的 ZOrder（层级基准 + 该层已有数量） */
    int32 GetZOrderForPush(EGameUILayer Layer) const;

private:
    /**
     * 活动界面列表（按推入顺序排列；各层级的"栈"即在此列表中该层级的相对顺序）。
     * 用单个扁平数组而非 TMap<Layer, TArray<>>：UHT 不支持"嵌套容器"作 UPROPERTY，
     * 而扁平结构同样能表达栈语义（同层取最后一项即栈顶），且 GC 追踪更直接。
     */
    UPROPERTY(Transient)
    TArray<FGameUIActiveWidget> ActiveWidgets;

    /** 路由表缓存（惰性加载后常驻，避免每次推入界面都重新加载资产） */
    UPROPERTY(Transient)
    TObjectPtr<UGameUIRouteTable> RouteTable;
};
