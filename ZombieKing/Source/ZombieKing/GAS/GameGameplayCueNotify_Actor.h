#pragma once

#include "CoreMinimal.h"
#include "GameplayCueNotify_Actor.h"    // AGameplayCueNotify_Actor（引擎"角色式" Cue 基类：有生命周期的持续表现）
#include "GameplayEffectTypes.h"        // EGameplayCueEvent::Type / FGameplayCueParameters（Cue 事件与参数）
#include "UnLuaInterface.h"             // IUnLuaInterface：把 Cue 通知资产绑定到 Lua 表现脚本（可热更）
#include "GameGameplayCueNotify_Actor.generated.h"

/**
 * 竞技生化网游：角色式 GameplayCue 通知基类（有生命周期的持续表现）+ Lua 桥
 *
 * 与静态版本（UGameGameplayCueNotify_Static）的分工：
 *  - 静态：一次性"爆发"表现（命中火花、音效），不实例化 Actor；
 *  - 本类（角色式）：**需要跟随 / 持续 / 可复用**的表现，引擎会实例化一个 Actor，
 *    支持 OnActive（首次激活）→ WhileActive（持续存在，含中途加入者也可见）→ OnRemove（移除）三阶段，
 *    并可在结束时回收复用（Recycle）。典型用途：
 *      · 被感染后挂在身上的持续生化特效；
 *      · 持续燃烧 / 减速 / 强化等状态的循环表现；
 *      · 需要跟随目标骨骼挂载的特效（引擎提供 AttachToComponent 支持）。
 *
 * 定位（对齐项目的"表现下沉 Lua"纪律）：
 *  - 本基类只做两件事：① IUnLuaInterface 绑定 Lua 脚本；② 把引擎 Cue 分发统一收敛为**一个 Lua 事件**；
 *  - 具体表现（生成/销毁哪些组件、播放什么音效、如何跟随）全部写在 Lua 脚本里，可热更。
 *
 * 事件顺序约定（重要）：
 *  - **先派发 Lua 事件，再调用 Super**。原因：引擎的 Actor 版本在 Removed 事件里会走
 *    GameplayCueFinishedCallback → 可能回收甚至销毁自身；若先调用 Super，Lua 就可能在
 *    "对象已被回收"之后才收到事件，导致清理/停止表现失败。
 *
 * 内容侧用法：
 *  - 在 `/Game/GameplayCues` 下建派生本类的通知资产（如 `GC_Zombie_Infected`），
 *    资产名由编辑器自动推导出 Cue 标签（规则见 GameplayCueTags.h 头注释）；
 *  - 在 `LuaScriptPath` 指定表现脚本模块；由 GE 资产（Added/AddedAndWhileActive/Removed 语义）
 *    驱动触发，从而保证广播范围与复制一致。
 *
 * 注意：本项目约定"表现由 Lua 接管"，故 Lua 侧实现 `OnGameplayCue`；请**不要**同时用蓝图实现
 *       引擎自带的 OnActive/OnExecute/OnRemove 等事件，以免同一事件被播放两次。
 */
UCLASS(Abstract, Blueprintable, hidecategories = (Replication))
class ZOMBIEKING_API AGameGameplayCueNotify_Actor : public AGameplayCueNotify_Actor,
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
     * @param MyTarget   Cue 作用的目标 Actor
     * @param EventType  引擎 Cue 事件类型：OnActive（首次激活）/ WhileActive（持续存在）/ Removed（移除）
     * @param Parameters 引擎 Cue 参数（含位置、法线、来源、效果上下文等）
     */
    UFUNCTION(BlueprintImplementableEvent, Category = "Lua|Cue")
    void OnGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters);

protected:
    /**
     * 覆写引擎的 Cue 分发入口：先派发 Lua 事件，再调用 Super 走引擎内部的
     * "激活 / 复用 / 结束回调"状态机（该状态机必须保留，否则 Cue 生命周期与回收会失效）。
     */
    virtual void HandleGameplayCue(AActor* MyTarget, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters) override;

    /** 绑定的 Lua 脚本模块路径（相对 Content/Script），可在各 Cue 通知资产中按需覆盖 */
    UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Scripting")
    FString LuaScriptPath;
};
