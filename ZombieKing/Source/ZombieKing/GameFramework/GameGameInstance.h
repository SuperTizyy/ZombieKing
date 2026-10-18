#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"   // UGameInstance（本类基类；注意与引擎同名类区分：本类为项目框架类）
#include "UnLuaInterface.h"        // IUnLuaInterface：把本 GameInstance 绑定到 Lua 框架脚本（可热更）
#include "GameGameInstance.generated.h"

/**
 * 竞技生化网游：全局游戏实例（最外层宿主，跨关卡存活）
 *
 * 架构定位（网游大厂分层）：
 *  - GameInstance 是进程内**跨关卡存活**的最外层 UObject：换地图、重开对局都不重建它；
 *    因此它是"服务层子系统"的天然宿主 —— 会话/房间、遥测、账号、设置等长期能力都挂在它下面
 *    （见 Source/ZombieKing/Subsystems/）；
 *  - 本类刻意保持"极薄"：只负责生命周期与 Lua 绑定，不承载任何业务规则；
 *    规则与数值一律下沉 Lua 脚本，满足竞技网游"不停机热更新"要求。
 *
 * 命名说明：
 *  - GameInstance 不是 Actor（不是 AActor 子类），因此前缀用 U 而非 A；
 *    "Game" 前缀沿用本项目框架层命名约定（对齐 AGameGameMode / AGameGameState 等）；
 *  - 类名不采用 UGameInstance 是因为它已被引擎占用。
 *
 * 装配方式（配置侧，已落地）：
 *  - Config/DefaultEngine.ini 的 [/Script/EngineSettings.GameMapsSettings] 中
 *    GameInstanceClass=/Script/ZombieKing.GameGameInstance，引擎启动时即实例化本类（或其蓝图子类）。
 *
 * Lua 绑定说明（与 UnLua 启动入口刻意分名，避免混淆）：
 *  - 本类的 Lua 模块为 Lua.Core.GameFramework.GameInstance（框架层脚本，承载 GameInstance 级逻辑）；
 *  - UnLua 的"启动入口脚本"是 Lua.Core.Bootstrap（见 Config/DefaultUnLuaSettings.ini），
 *    只做调试环境引导，与本类无关。
 */
UCLASS()
class ZOMBIEKING_API UGameGameInstance : public UGameInstance,
                                         public IUnLuaInterface
{
    GENERATED_BODY()

public:
    UGameGameInstance();

    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 返回本 GameInstance 绑定的 Lua 脚本模块路径（相对 Content/Script） */
    // 注意：UnLua 的接口声明为 const（IUnLuaInterface::GetModuleName() const），签名必须完全一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // 生命周期（C++ 机制层：只做"通知 + 日志"，不做业务规则）
    // -------------------------------------------------------------------------
    /**
     * 进程启动：引擎创建本实例后调用。
     * 顺序约定：Super::Init() 内部会创建并初始化全部 GameInstanceSubsystem（即服务层），
     *          因此**必须先调用 Super 再派发 Lua 事件**，否则 Lua 在事件里取不到各服务。
     */
    virtual void Init() override;

    /**
     * 进程关闭：引擎在退出前调用。
     * 顺序约定：**先派发 Lua 关闭事件再调用 Super**，让 Lua 在各服务/世界被销毁之前
     *          完成存档落盘、遥测冲刷等收尾动作。
     */
    virtual void Shutdown() override;

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent，UnLua 绑定脚本同名函数自动接管）
    // -------------------------------------------------------------------------
    /** 全局初始化完成（服务层已就绪）：Lua 在此加载全局配置 / 预热资源 / 拉取远端公告等 */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|GameInstance")
    void OnGameInstanceInit();

    /** 全局关闭前：Lua 在此落盘存档 / 冲刷遥测 / 上报会话结束等收尾 */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|GameInstance")
    void OnGameInstanceShutdown();

    /**
     * 语言已切换（C++ 已把语言落地到引擎 culture 之后派发）。
     * Lua 在此重载 Lua 文案表、刷新界面文案 —— 即"引擎管不到的那部分文案"的自愈入口。
     * 说明：引擎会自动刷新由 FText（本地化表）驱动的界面文案，但 Lua 文案表与自行拼接的字符串不会，
     *       故必须提供本事件；启动期也会派发一次（带上当前语言），供 Lua 完成首次装载。
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|GameInstance")
    void OnLanguageChanged(const FString& NewCultureCode);

protected:
    /** 绑定的 Lua 脚本模块路径（相对 Content/Script），可在蓝图子类中覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

private:
    /**
     * 本地化服务"语言已切换"事件的 C++ 处理：只做"转发给 Lua"，不写任何规则。
     * 签名注意：动态多播委托的绑定函数必须与委托参数声明**逐字一致**（委托声明为 `FString` 值传递），
     *           故这里也用 `FString` 值传递，不能写成 `const FString&`（否则 AddDynamic 编译不过）。
     */
    UFUNCTION()
    void HandleLanguageChanged(FString NewCultureCode);
};
