#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"     // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameTelemetrySubsystem.generated.h"

/**
 * 一条遥测事件（埋点数据的基本单位）。
 * 说明：字段刻意保持"通用 + 可热更"：事件名与载荷内容全部由调用方（多来自 Lua 规则脚本）决定，
 *       因此新增埋点类型不需要改 C++、不需要重编译。
 */
USTRUCT(BlueprintType)
struct FGameTelemetryEvent
{
    GENERATED_BODY()

    /** 事件名：建议用稳定的分类名（如 "Match.Kill" / "Infection.Triggered"），便于后端聚合 */
    UPROPERTY(BlueprintReadWrite, Category = "Telemetry")
    FName EventName;

    /** 事件发生时的服务器时间（秒）；调用方未填写（<=0）时由遥测服务自动补当前时间 */
    UPROPERTY(BlueprintReadWrite, Category = "Telemetry")
    double ServerTimeSeconds = 0.0;

    /** 事件载荷（JSON 字符串）：结构化细节字段，其含义由事件名约定（如 {"killer":..,"victim":..}） */
    UPROPERTY(BlueprintReadWrite, Category = "Telemetry")
    FString PayloadJson;
};

/** 遥测事件广播（供后端上报模块 / Lua 表现层订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameTelemetryEvent, const FGameTelemetryEvent&, Event);

/**
 * 竞技生化网游：遥测 / 埋点服务（GameInstance 层服务）
 *
 * 职责边界：
 *  - 本服务只做"机制"：接收事件 → 补齐时间戳 → 写入有上限的本地缓冲 → 广播给订阅方；
 *  - "埋哪些点、字段怎么定义、是否上报"等**规则/策略**由 Lua 规则脚本与后端约定决定（可热更）；
 *  - 当前未接入正式后端：事件只留在本地缓冲并广播，正式上报（批量/异步/重试）见 TODO。
 *
 * 为什么要有缓冲：竞技对局事件量大且高峰集中，直接逐条上传会阻塞主流程；
 *   缓冲 + 批量上报是业界常规做法，故本服务从第一天就预留缓冲结构（含容量上限，防止长对局内存无界增长）。
 */
UCLASS()
class ZOMBIEKING_API UGameTelemetrySubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 开关与查询
    // -------------------------------------------------------------------------
    /** 设置是否启用遥测采集（开发期可关、发布期开；由 Lua 或配置文件驱动） */
    UFUNCTION(BlueprintCallable, Category = "Telemetry")
    void SetTelemetryEnabled(bool bEnabled) { bTelemetryEnabled = bEnabled; }

    /** 当前是否启用遥测采集 */
    UFUNCTION(BlueprintPure, Category = "Telemetry")
    bool IsTelemetryEnabled() const { return bTelemetryEnabled; }

    // -------------------------------------------------------------------------
    // 事件记录与读取
    // -------------------------------------------------------------------------
    /**
     * 记录一条遥测事件（BlueprintCallable：C++ 与 Lua 统一从这一入口埋点）。
     * @param Event 事件数据（按值传入）；其中 ServerTimeSeconds <= 0 时由本服务自动补当前服务器时间
     */
    UFUNCTION(BlueprintCallable, Category = "Telemetry")
    void RecordEvent(FGameTelemetryEvent Event);

    /** 取当前缓冲的全部事件（供后端上报模块或 Lua 读取；只读，勿直接修改） */
    UFUNCTION(BlueprintPure, Category = "Telemetry")
    const TArray<FGameTelemetryEvent>& GetBufferedEvents() const { return BufferedEvents; }

    /** 清空缓冲（通常在上报成功或被丢弃后调用） */
    UFUNCTION(BlueprintCallable, Category = "Telemetry")
    void ClearBufferedEvents();

    // -------------------------------------------------------------------------
    // 事件广播（BlueprintAssignable：后端上报模块 / Lua 订阅）
    // -------------------------------------------------------------------------
    /** 每记录一条事件即广播一次（携带该事件数据） */
    UPROPERTY(BlueprintAssignable, Category = "Telemetry")
    FOnGameTelemetryEvent OnTelemetryEvent;

protected:
    /** 启动：复位缓冲与开关到默认状态 */
    virtual void OnServiceInitialize() override;

    /** 关闭：清空缓冲（尚未上报的事件会丢弃；正式版本应在此处做一次同步冲刷） */
    virtual void OnServiceDeinitialize() override;

private:
    /** 事件缓冲（有容量上限；用 Transient 标记，避免被序列化到存档/复制） */
    UPROPERTY(Transient)
    TArray<FGameTelemetryEvent> BufferedEvents;

    /** 是否启用采集 */
    bool bTelemetryEnabled = true;

    /** 缓冲容量上限：超出后丢弃最旧事件，防止长时间对局导致内存无界增长（可在蓝图子类调参） */
    UPROPERTY(EditDefaultsOnly, Category = "Telemetry")
    int32 MaxBufferedEvents = 1024;
};
