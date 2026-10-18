#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"   // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameFlowSubsystem.generated.h"

/**
 * 竞技生化网游：前端流程阶段（客户端本地壳，4 阶段）
 *
 * 与"对局内阶段"的分工（重要，避免两套阶段概念打架）：
 *  - 本枚举 = **前端流程**：描述"玩家当前处在游戏外壳的哪一段"（启动 / 主菜单 / 房间 / 对局），
 *    它是客户端本地 UI 流程，随 GameInstance 存活，不参与网络复制；
 *  - 对局内阶段 = `AGameGameState` 上的 `EGameMatchPhase`（Lobby / Warmup / Infection /
 *    Combat / Settlement / Intermission）：**服务器权威**，描述"这一场对局打到哪一步"。
 *  - 二者是包含关系：流程处于 `Match` 时，世界内由 `EGameMatchPhase` 继续细分。
 *    结算界面不单列为流程阶段，而是"对局内的 UI"（由 Lua 在比赛结束事件里弹出）。
 */
UENUM(BlueprintType)
enum class EGameFlowStage : uint8
{
    /** 启动：引擎刚起来，尚未进入任何界面（加载配置、连后端等） */
    Boot     UMETA(DisplayName = "启动"),

    /** 主菜单：已进入游戏外壳的主界面 */
    MainMenu UMETA(DisplayName = "主菜单"),

    /** 房间：已进入某个房间（等待开局 / 选人 / 准备） */
    Room     UMETA(DisplayName = "房间"),

    /** 对局：已进入对局世界 */
    Match    UMETA(DisplayName = "对局")
};

/** 前端流程阶段变化广播（供 UI 脚本、音效、遥测等订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameFlowStageChanged, EGameFlowStage, NewStage, EGameFlowStage, OldStage);

/**
 * 竞技生化网游：前端流程服务（GameInstance 层服务）
 *
 * 职责边界（严格遵循"机制在 C++、规则在 Lua"）：
 *  - 本服务只维护**当前流程阶段**并提供迁移接口 + 变化事件；
 *  - **迁移合法性完全不做校验**：允许从哪走到哪、什么条件才切、切的时候弹哪个界面，
 *    全部写在 Lua 规则脚本里（可热更）—— 这正是"规则下沉"的落点；
 *  - 本服务不创建界面（界面由 UGameUISubsystem 管），两类职责互不越界。
 *
 * 为什么放在 GameInstance 层（而非 GameState）：流程跨越"主菜单（无对局世界）"与"对局（有世界）"
 * 两个阶段，必须随 GameInstance 存活，否则换图/进出对局时流程状态会丢。
 */
UCLASS()
class ZOMBIEKING_API UGameFlowSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 查询
    // -------------------------------------------------------------------------
    /** 当前流程阶段 */
    UFUNCTION(BlueprintPure, Category = "Flow")
    EGameFlowStage GetFlowStage() const { return FlowStage; }

    /** 是否已在对局中（世界内阶段请另查 GameState 的 EGameMatchPhase） */
    UFUNCTION(BlueprintPure, Category = "Flow")
    bool IsInMatch() const { return FlowStage == EGameFlowStage::Match; }

    /** 是否处在"前端"（主菜单或房间；即尚未进入对局） */
    UFUNCTION(BlueprintPure, Category = "Flow")
    bool IsInFrontEnd() const { return FlowStage == EGameFlowStage::MainMenu || FlowStage == EGameFlowStage::Room; }

    // -------------------------------------------------------------------------
    // 阶段迁移（BlueprintCallable：供 Lua 规则脚本驱动）
    // -------------------------------------------------------------------------
    /**
     * 迁移前端流程阶段。
     * 语义：**幂等**设置（阶段相同则不广播）。
     * 注意：本函数刻意不做"迁移是否合法"的校验 —— 那是规则，写在 Lua 里；
     *       若 Lua 写了非法跳转，问题应在规则脚本侧修正，而不是在这里加硬编码限制。
     */
    UFUNCTION(BlueprintCallable, Category = "Flow")
    void SetFlowStage(EGameFlowStage NewStage);

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：Lua / UI 脚本 / 音效 / 遥测订阅）
    // -------------------------------------------------------------------------
    /** 流程阶段变化事件（仅在阶段真正变化时广播一次） */
    UPROPERTY(BlueprintAssignable, Category = "Flow")
    FOnGameFlowStageChanged OnFlowStageChanged;

protected:
    /** 启动：流程回到起点（Boot）——进程刚起来时必然处于启动阶段 */
    virtual void OnServiceInitialize() override;

private:
    /** 当前流程阶段 */
    EGameFlowStage FlowStage = EGameFlowStage::Boot;
};
