#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Static.h"   // UGameplayCueNotify_Static（引擎静态 Cue 基类：一次性"爆发"表现）
#include "GameplayEffectTypes.h"        // EGameplayCueEvent::Type / FGameplayCueParameters（Cue 事件与参数）
#include "UnLuaInterface.h"             // IUnLuaInterface：把 Cue 通知资产绑定到 Lua 表现脚本（可热更）
#include "GameGameplayCueNotify_Static.generated.h"

/**
 * 竞技生化网游：静态 GameplayCue 通知基类（一次性表现）+ Lua 桥
 *
 * 定位（对齐项目的"表现下沉 Lua"纪律）：
 *  - 引擎的静态 Cue（UGameplayCueNotify_Static）用于**一次性爆发表现**：命中火花、命中音效、
 *    弹道拖尾、镜头震动等——由 GameplayCue 通道统一分发，不需要实例化 Actor；
 *  - 本基类只做两件事：① 通过 IUnLuaInterface 把 Cue 通知资产绑定到 Lua 脚本；
 *    ② 把引擎的 Cue 分发统一收敛为**一个 Lua 事件**（OnGameplayCue），避免 Lua 侧要分别实现
 *    四个引擎事件；
 *  - 真正的表现内容（用什么粒子、什么音效、镜头怎么抖）全部写在 Lua 脚本里 —— 改完即生效、可热更。
 *
 * 事件顺序约定（重要）：
 *  - 先派发 Lua 事件、再调用 Super（引擎分发）。原因是"移除"事件下引擎的 Actor 版本可能回收/销毁
 *    自身；静态版本虽无此问题，但统一顺序可让 Lua 始终在"对象仍有效"的时刻拿到事件。
 *
 * 内容侧用法：
 *  - 在 `/Game/GameplayCues` 下建派生本类的**通知资产**（如 `GC_Damage_Head`），
 *    资产名会由编辑器自动推导出 Cue 标签（规则见 GameplayCueTags.h 头注释）；
 *  - 在 `LuaScriptPath` 上指定该 Cue 的表现脚本模块（不填则用默认共享脚本，仅打日志）；
 *  - 由 GE 资产驱动：把 Cue 标签填进伤害 GE 的 `GameplayCues` 数组，引擎便会在服务端与
 *    各相关客户端自动触发（**推荐路径**：走 GE 可保证广播范围与复制一致，
 *    不要在各业务代码里手工触发，否则容易重复播放或漏播）。
 *
 * 注意：本项目约定"表现由 Lua 接管"，故 Lua 侧实现 `OnGameplayCue`；请**不要**同时用蓝图实现
 *       引擎自带的 OnExecute/OnActive 等事件，以免同一事件被播放两次。
 */
UCLASS(Abstract, Blueprintable)
class ZOMBIEKING_API UGameGameplayCueNotify_Static : public UGameplayCueNotify_Static,
                                                      public IUnLuaInterface
{
    GENERATED_BODY()

public:
    // -------------------------------------------------------------------------
    // UnLua 绑定接口
    // -------------------------------------------------------------------------
    /** 返回本通知资产绑定的 Lua 脚本模块路径（相对 Content/Script） */
    // 注意：UnLua 的接口声明为 const（IUnLuaInterface::GetModuleName() const），签名必须完全一致
    virtual FString GetModuleName_Implementation() const override;

    // -------------------------------------------------------------------------
    // Lua 事件契约（BlueprintImplementableEvent；UnLua 绑定脚本同名函数自动接管）
    // -------------------------------------------------------------------------
    /**
     * Cue 分发统一入口（Lua 侧实现同名函数即接管全部 Cue 事件）。
     * @param MyTarget   Cue 作用的目标 Actor（通常是被命中 / 死亡 / 变异的角色）
     * @param EventType  引擎 Cue 事件类型：Executed（一次性执行）/ OnActive / WhileActive / Removed
     * @param Parameters 引擎 Cue 参数（含位置、法线、来源、效果上下文等；命中细节可从
     *                   Parameters.EffectContext.GetHitResult() 取）
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Cue")
    void OnGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters);

protected:
    /**
     * 覆写引擎的 Cue 分发入口：先派发 Lua 事件，再调用 Super 走引擎内部分发。
     * 之所以覆写而非直接用引擎事件：本项目所有 Actor/控件都走"UnLua 绑定 + 同名事件接管"的约定，
     * 这里保持一致，Lua 侧只需实现一个函数即可覆盖全部 Cue 事件类型。
     */
    virtual void HandleGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters) override;

    /** 绑定的 Lua 脚本模块路径（相对 Content/Script），可在各 Cue 通知资产中按需覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;
};
