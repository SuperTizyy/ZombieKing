#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "UnLuaInterface.h"           // IUnLuaInterface：把 HUD 控件绑定到 Lua 表现脚本
#include "GameFaction.h"              // EGameFaction（阵营显示 / 准星变色）
#include "GameGameState.h"            // EGameMatchPhase（阶段显示 / 倒计时）
#include "GameHUDWidget.generated.h"

class AGamePlayerState;               // 玩家状态（阵营 + ASC 生命属性）
class AGameGameState;                 // 对局状态（阶段 / 比分）
struct FOnAttributeChangeData;        // GAS 属性变化数据（仅作回调形参，头文件只需前向声明）

/**
 * 竞技生化网游：HUD 控件基类（C++ 契约骨架 + Lua 表现）
 *
 * 分层职责（工业级 UnLua 约定）：
 *  - 本类（C++）只做"机制"两件事：
 *      ① 生命周期内把 HUD 关注的数据源接上（玩家生命属性、阵营、对局阶段与比分），
 *         并在数据源变化时幂等重绑（应对"控件先于 PlayerState 复制而创建"的时序问题）；
 *      ② 把数据变化转发为 BlueprintImplementableEvent，交由 Lua 接管显示逻辑。
 *  - Lua 层（Content/Script/Lua/Core/UI/GameHUDWidget.lua）：血条动画、倒计时数字、
 *     比分板、阵营提示、受击反馈等全部"表现与提示"逻辑——改完即生效、可热更。
 *
 * 数据源选择（为什么绑 PlayerState 而不是 Pawn）：
 *  - 生命属性住在 PlayerState 托管的 ASC 上（Pawn 只是短生命周期 Avatar），
 *    因此直接绑 PlayerState 的 ASC 属性委托后，感染变异 / 死亡重生都无需重绑；
 *  - 阵营在 PlayerState 上（OnPlayerFactionChanged），对局阶段与比分在 GameState 上，三者都是长寿命对象。
 *
 * 内容侧：用控件蓝图（WBP）派生本类搭建界面；纯 C++ 无法直接使用（无视觉树）。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UGameHUDWidget : public UUserWidget,
                                       public IUnLuaInterface
{
    GENERATED_BODY()

public:
    UGameHUDWidget(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 返回本控件绑定的 Lua 脚本模块路径（相对 Content/Script，层级用 . 连接） */
    // 注意：UnLua 2.3.6 的接口声明为 const，签名必须完全一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // 控件生命周期（C++ 机制层：只负责订阅/解绑数据源，不写显示逻辑）
    // -------------------------------------------------------------------------
    /** 控件构造：尝试首次绑定数据源（PlayerState 可能尚未复制到位，由 NativeTick 兜底重试） */
    virtual void NativeConstruct() override;

    /** 控件析构：解绑全部订阅，防止控件销毁后回调悬空 */
    virtual void NativeDestruct() override;

    /**
     * 每帧回调（仅"展示层"用途，且开销极小）：
     *  1) 幂等校验数据源绑定（仅在 PlayerState / GameState 实例发生变化时才重绑）；
     *  2) 转发 OnHUDTick 给 Lua，用于倒计时等"连续变化"的显示刷新。
     */
    virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

    // -------------------------------------------------------------------------
    // 只读访问（BlueprintPure：供 Lua / 蓝图取值；事件只报"变化了"，取值仍由表现层自取）
    // -------------------------------------------------------------------------
    /** 取本机玩家的状态对象（内含阵营与 GAS 生命属性）；未就绪时为 null */
    UFUNCTION(BlueprintPure, Category = "HUD|Context")
    AGamePlayerState* GetGamePlayerState() const;

    /** 取对局状态对象（内含阶段 / 阶段结束时间 / 比分）；未就绪时为 null */
    UFUNCTION(BlueprintPure, Category = "HUD|Context")
    AGameGameState* GetGameGameState() const;

    /** 当前生命值（未接入时返回 0） */
    UFUNCTION(BlueprintPure, Category = "HUD|Health")
    float GetHealth() const;

    /** 当前生命上限（未接入时返回 0） */
    UFUNCTION(BlueprintPure, Category = "HUD|Health")
    float GetMaxHealth() const;

    /** 生命百分比 0..1（上限为 0 时返回 0，供血条直接使用） */
    UFUNCTION(BlueprintPure, Category = "HUD|Health")
    float GetHealthPercent() const;

    /** 阶段剩余秒数（透传 GameState；<=0 或大厅无限等待时为 0） */
    UFUNCTION(BlueprintPure, Category = "HUD|Phase")
    float GetMatchPhaseRemainingTime() const;

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent；Lua 脚本定义同名函数即自动接管）
    // 说明：这些事件只告知"发生了什么/新值是多少"，具体如何显示由 Lua 决定。
    // -------------------------------------------------------------------------
    /** 生命值 / 生命上限变化（首次绑定成功时也会推送一次当前值，用于初始化显示） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|HUD")
    void OnHUDHealthChanged(float NewHealth, float MaxHealth, float Delta);

    /** 阵营变化（首次绑定时 OldFaction 与 NewFaction 相同，表现为"当前阵营"） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|HUD")
    void OnHUDFactionChanged(EGameFaction NewFaction, EGameFaction OldFaction);

    /** 对局阶段变化（首次绑定时 OldPhase 与 NewPhase 相同，表现为"当前阶段"） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|HUD")
    void OnHUDMatchPhaseChanged(EGameMatchPhase NewPhase, EGameMatchPhase OldPhase);

    /** 阵营比分变化（无参数：Lua 收到后调用 GetGameGameState():GetTeamScore 取最新值） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|HUD")
    void OnHUDTeamScoresChanged();

    /** 每帧表现刷新钩子（用于倒计时数字、进度条插值等连续变化） */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|HUD")
    void OnHUDTick(float DeltaSeconds);

protected:
    // -------------------------------------------------------------------------
    // 数据源订阅（机制层；每个数据源独立绑定/解绑，避免相互误拆）
    // -------------------------------------------------------------------------
    /** 幂等刷新绑定：仅当数据源实例发生变化时才重绑，并在重绑后推送一次当前值供 Lua 初始化显示 */
    void RefreshDataSources();

    /** 绑定 / 解绑玩家状态（阵营委托 + ASC 生命属性委托） */
    void BindPlayerState(AGamePlayerState* InPlayerState);
    void UnbindPlayerState();

    /** 绑定 / 解绑对局状态（阶段委托 + 比分委托） */
    void BindGameState(AGameGameState* InGameState);
    void UnbindGameState();

    // --- 动态委托回调（BlueprintAssignable 委托必须用 UFUNCTION 接收） ---
    UFUNCTION() void HandlePlayerFactionChanged(EGameFaction NewFaction, EGameFaction OldFaction);
    UFUNCTION() void HandleMatchPhaseChanged(EGameMatchPhase NewPhase, EGameMatchPhase OldPhase);
    UFUNCTION() void HandleTeamScoresChanged();

    // --- ASC 属性委托回调（非动态委托，签名遵循 FOnAttributeChangeData） ---
    void HandleHealthAttributeChanged(const FOnAttributeChangeData& Data);
    void HandleMaxHealthAttributeChanged(const FOnAttributeChangeData& Data);

    // -------------------------------------------------------------------------
    // 配置
    // -------------------------------------------------------------------------
    /** 绑定的 Lua 脚本模块路径（相对 Content/Script），可在控件蓝图子类中覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;

private:
    /** 已绑定的玩家状态（弱引用：用于幂等重绑判定，且避免悬空） */
    TWeakObjectPtr<AGamePlayerState> BoundPlayerState;

    /** 已绑定的对局状态（弱引用） */
    TWeakObjectPtr<AGameGameState> BoundGameState;

    /** ASC 属性委托句柄：解绑时据此精确移除（非动态委托无法用 RemoveDynamic） */
    FDelegateHandle HealthChangedDelegateHandle;
    FDelegateHandle MaxHealthChangedDelegateHandle;
};
