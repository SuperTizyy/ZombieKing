#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"   // UGameInstanceSubsystem（本类基类：随 GameInstance 生命周期存活）
#include "GameServiceSubsystem.generated.h"

/**
 * 竞技生化网游：服务子系统统一基类（GameInstance 层"服务"契约）
 *
 * 定位（对齐网游大厂的服务层分层）：
 *  - 本项目把"跨关卡存活、与某一场对局无关"的长期能力统称为**服务**（Service），
 *    一律做成 GameInstanceSubsystem 并继承本类：会话/房间、遥测/埋点、账号、设置等；
 *  - 本基类不承载任何业务逻辑（业务写在各子类 + Lua 脚本），只收敛三件事：
 *      ① 统一生命周期契约：把引擎的 Initialize/Deinitialize 收敛为子类只需实现的
 *         OnServiceInitialize/OnServiceDeinitialize 两个钩子（避免子类忘记调用 Super 而破坏引擎流程）；
 *      ② 统一可查询性：GetServiceName 提供稳定短名，供日志前缀与按名定位使用；
 *      ③ 统一就绪标志：IsServiceReady 让外部在服务尚未初始化完成时能安全跳过调用。
 *
 * 与 World 层服务的关系（重要，避免误解）：
 *  - 生命周期绑定"世界（一场对局）"的服务（例如反作弊快照）**不能**继承本类：
 *    C++ 单继承，而引擎的 UWorldSubsystem 与本类并无继承关系，二者只能各选其一；
 *  - 因此本项目的约定是：GameInstance 层服务继承本类；World 层服务直接派生 UWorldSubsystem；
 *    两类服务的共同纪律是"只做机制、规则与数值下沉 Lua（可热更）"，而非共享同一个 C++ 基类。
 *
 * 热更新纪律：本类里不写任何规则与数值；服务若要驱动可调规则，一律通过
 *            BlueprintCallable 接口 + BlueprintAssignable 委托暴露给 Lua 脚本决定。
 */
UCLASS(Abstract)
class ZOMBIEKING_API UGameServiceSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    // -------------------------------------------------------------------------
    // 生命周期（引擎入口 → 受控钩子）
    // -------------------------------------------------------------------------
    /**
     * 引擎在 GameInstance 初始化阶段创建本服务并调用本函数。
     * 注意：引擎调用本函数时，同一 GameInstance 上的其它服务**可能尚未创建**，
     *       因此子类在 OnServiceInitialize 中不得假定"别的服务已经就绪"（要按需延迟取用）。
     */
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;

    /** 引擎在 GameInstance 关闭（或本服务被移除）时调用；用于释放外部资源、解绑委托 */
    virtual void Deinitialize() override;

    // -------------------------------------------------------------------------
    // 服务标识与就绪状态
    // -------------------------------------------------------------------------
    /**
     * 返回本服务的稳定短名（用于日志前缀与按名定位，例如 "Session" / "Telemetry"）。
     * 子类必须实现；返回值一经发布即作为对外索引，不得随意变更。
     */
    virtual FName GetServiceName() const PURE_VIRTUAL(UGameServiceSubsystem::GetServiceName, return NAME_None;);

    /** 本服务是否已完成初始化（外部调用服务接口前可据此安全判断，避免半初始化状态被访问） */
    UFUNCTION(BlueprintPure, Category = "Service")
    bool IsServiceReady() const { return bServiceReady; }

protected:
    // -------------------------------------------------------------------------
    // 子类实现点（业务只写在这里，不要覆盖 Initialize/Deinitialize）
    // -------------------------------------------------------------------------
    /** 服务启动：子类在此申请资源、注册委托；默认空实现 */
    virtual void OnServiceInitialize() {}

    /** 服务关闭：子类在此释放资源、解绑委托；默认空实现 */
    virtual void OnServiceDeinitialize() {}

    /**
     * 统一日志输出（自动带 "[服务:<短名>]" 前缀）。
     * 目的：让 LogZombieKing 的输出能一眼分辨是哪个服务在说话，便于线上排障与日志过滤。
     */
    void LogService(const FString& Message) const;

private:
    /**
     * 就绪标志。
     * 只由基类在 Initialize/Deinitialize 中维护（子类不要直接改写），
     * 从而保证"标志含义"在所有服务上完全一致，外部判断才可信。
     */
    bool bServiceReady = false;
};
