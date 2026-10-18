#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"   // UUserWidget（UMG 控件基类）
#include "UnLuaInterface.h"         // IUnLuaInterface：把界面控件绑定到 Lua 脚本（表现逻辑可热更）
#include "GameUITypes.h"            // EGameUILayer（本界面所处层级）
#include "GameUserWidgetBase.generated.h"

class UGameUISubsystem;             // UI 管理层（界面脚本常用它来关闭自己 / 推入新界面）

/**
 * 竞技生化网游：前端界面基类（主菜单 / 设置 / 房间等界面的共同父类）
 *
 * 与对局内 HUD 的分工：
 *  - `UGameHUDWidget`（对局内）：绑 PlayerState / GameState 的运行时数据（血量、阶段、比分）；
 *  - 本类（前端界面）：只关心"自己是谁（路由）、在第几层、什么时候显示/隐藏"，
 *    数据来源由各界面脚本自己去取（多数来自会话服务 / 流程服务）。
 *
 * 生命周期与 Lua 桥（与项目其它 UnLua 桥一致：C++ 派发事件、Lua 实现同名函数）：
 *  - `OnWidgetShow`  —— 界面加入视口后派发（初始化显示、拉取数据、播放入场动画）；
 *  - `OnWidgetHide`  —— 界面移出视口前派发（停止动画、释放临时资源）；
 *  - 两者均由 UI 管理层（UGameUISubsystem）触发的控件生命周期驱动，界面脚本无需关心 Slate 细节。
 *
 * 内容侧：用控件蓝图（WBP）派生本类搭建界面；`LuaScriptPath` 未填写时回落到默认共享脚本。
 */
UCLASS(Abstract, Blueprintable)
class ZOMBIEKING_API UGameUserWidgetBase : public UUserWidget,
                                           public IUnLuaInterface
{
    GENERATED_BODY()

public:
    UGameUserWidgetBase(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 返回本界面绑定的 Lua 脚本模块路径（相对 Content/Script） */
    // 注意：UnLua 的接口声明为 const（IUnLuaInterface::GetModuleName() const），签名必须完全一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // 路由信息（由 UI 管理层在创建后写入，界面脚本只读）
    // -------------------------------------------------------------------------
    /** 写入路由与层级（仅 UI 管理层调用；界面自身不应改动） */
    void SetRouteInfo(FName InRoute, EGameUILayer InLayer);

    /** 本界面的路由名 */
    UFUNCTION(BlueprintPure, Category = "UI")
    FName GetRoute() const { return Route; }

    /** 本界面所处层级 */
    UFUNCTION(BlueprintPure, Category = "UI")
    EGameUILayer GetUILayer() const { return Layer; }

    /** 取 UI 管理层（界面脚本用它关闭自己、或推入/弹出其它界面） */
    UFUNCTION(BlueprintPure, Category = "UI")
    UGameUISubsystem* GetUISubsystem() const;

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent；UnLua 绑定脚本同名函数自动接管）
    // -------------------------------------------------------------------------
    /** 界面显示（已加入视口） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|UI")
    void OnWidgetShow();

    /** 界面隐藏（即将移出视口） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|UI")
    void OnWidgetHide();

protected:
    /** 控件构造完成（AddToViewport 后触发）：先调 Super，再派发 Lua 显示事件 */
    virtual void NativeConstruct() override;

    /** 控件析构（RemoveFromParent 时触发）：先派发 Lua 隐藏事件（此时控件仍有效），再调 Super */
    virtual void NativeDestruct() override;

    /** 本界面绑定的 Lua 脚本模块路径（相对 Content/Script），可在各 WBP 中覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

private:
    /** 本界面的路由名（由 UI 管理层写入） */
    UPROPERTY(BlueprintReadOnly, Category = "UI", meta = (AllowPrivateAccess = "true"))
    FName Route;

    /** 本界面所处层级（由 UI 管理层写入） */
    UPROPERTY(BlueprintReadOnly, Category = "UI", meta = (AllowPrivateAccess = "true"))
    EGameUILayer Layer = EGameUILayer::Window;
};
