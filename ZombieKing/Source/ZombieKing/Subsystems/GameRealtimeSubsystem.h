#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"     // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "Containers/Ticker.h"        // FTSTicker（机制层定时器/重连调度）
#include "Templates/SharedPointer.h"
#include "GameRealtimeSubsystem.generated.h"

class IWebSocket;                     // 引擎 WebSocket 接口（仅实现文件使用）

/** 实时事件广播（原始 rtapi Envelope JSON；由 Lua 规则层按字段分派：channel_message / match_data / status_presence_event 等） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRealtimeEvent, const FString&, EnvelopeJson);

/** 连接状态变化广播 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRealtimeConnectionChanged, bool, bConnected);

/** 周期事件广播（机制层定时器到点；Name 为 StartRepeating 注册的名字，Lua 按名分派心跳/轮询） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnRealtimeRepeating, const FString&, Name);

/**
 * 竞技生化网游：实时通道服务（GameInstance 层服务）——Nakama WebSocket 的**纯机制**封装
 *
 * 定位（与 UGameBackendSubsystem 对称：一个管 HTTP 唯一出口，一个管 WS 唯一出口）：
 *  - 连接/断开/自动重连（非主动断开每 5s 重试，令牌失效由服务端断连、上层重新登录后再 Connect）；
 *  - 信封收发：Lua 组装 rtapi Envelope（协议字段见 GameBackendEndpoints 头注释），
 *    机制层负责注入自增 cid、把服务端回包原文广播给订阅方；
 *  - 机制定时器：StartRepeating/StopRepeating + OnRepeating 事件，供 Lua 驱动心跳与轮询
 *    （GameInstance 层无 WorldTimerManager，故机制层自带 ticker）。
 *
 * 纪律：本服务不理解任何业务语义（不认识"聊天/房间/撮合"），只搬运 JSON；
 *       "发什么、收到怎么处置"全部在 Lua 规则层（可热更）。
 *
 * 线程约定：引擎 WebSocket 回调在游戏线程投递（FWebSocketsModule 默认），可直接操作 UObject 状态。
 */
UCLASS()
class ZOMBIEKING_API UGameRealtimeSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 连接管理
    // -------------------------------------------------------------------------
    /**
     * 以会话令牌连接实时通道（已连接时先断开再重连）。
     * @param SessionToken Nakama 登录下发的 access token（拼入 /ws?token=）
     * @return 是否已发起连接（false = 令牌为空或配置缺失）
     */
    UFUNCTION(BlueprintCallable, Category = "Realtime")
    bool Connect(const FString& SessionToken);

    /** 主动断开（不再自动重连） */
    UFUNCTION(BlueprintCallable, Category = "Realtime")
    void Disconnect();

    /** 当前是否已连接 */
    UFUNCTION(BlueprintPure, Category = "Realtime")
    bool IsConnected() const { return bOpen; }

    // -------------------------------------------------------------------------
    // 信封收发
    // -------------------------------------------------------------------------
    /**
     * 发送一个 rtapi Envelope（命令对象 JSON，不含 cid；机制层注入自增 cid）。
     * 例：{"channel_join":{"target":"<group_id>","type":3}}
     * @return 本次发送的 cid（空串 = 未连接或参数非法）
     */
    UFUNCTION(BlueprintCallable, Category = "Realtime")
    FString SendEnvelope(const FString& CmdObjectJson);

    // -------------------------------------------------------------------------
    // 机制定时器（Lua 驱动心跳/轮询用；名字唯一，重复注册视为覆盖）
    // -------------------------------------------------------------------------
    /** 注册周期事件：到点广播 OnRepeating(Name) */
    UFUNCTION(BlueprintCallable, Category = "Realtime|Timer")
    void StartRepeating(const FString& Name, float IntervalSeconds);

    /** 注销周期事件 */
    UFUNCTION(BlueprintCallable, Category = "Realtime|Timer")
    void StopRepeating(const FString& Name);

    // -------------------------------------------------------------------------
    // 事件（BlueprintAssignable：Lua / UI 订阅）
    // -------------------------------------------------------------------------
    /** 服务端 Envelope 原文事件（含回包与推送；按 JSON 字段分派） */
    UPROPERTY(BlueprintAssignable, Category = "Realtime")
    FOnRealtimeEvent OnEvent;

    /** 连接状态变化事件 */
    UPROPERTY(BlueprintAssignable, Category = "Realtime")
    FOnRealtimeConnectionChanged OnConnectionChanged;

    /** 周期事件（对应 StartRepeating 注册的名字） */
    UPROPERTY(BlueprintAssignable, Category = "Realtime|Timer")
    FOnRealtimeRepeating OnRepeating;

protected:
    virtual void OnServiceInitialize() override;
    virtual void OnServiceDeinitialize() override;

private:
    /** 建立底层 socket 并绑定回调 */
    void OpenSocket();

    /** 收到文本帧（游戏线程） */
    void HandleIncomingMessage(const FString& Message);

    /** 连接关闭（游戏线程）：非主动断开则安排重连 */
    void HandleConnectionClosed();

    /** 重连 tick（ticker 回调） */
    void TryReconnect();

private:
    /** 底层 socket（引擎接口，仅实现层持有） */
    TSharedPtr<IWebSocket> Socket;

    /** 连接令牌（重连复用；Disconnect 主动断开时清空） */
    FString Token;

    /** 当前连接状态（OnConnectionChanged 去重依据） */
    bool bOpen = false;

    /** 是否主动断开（主动断开不自动重连） */
    bool bExplicitDisconnect = false;

    /** 信封 cid 分配器（进程内自增即可，服务端只要求同连接内可区分） */
    int32 NextCid = 1;

    /** 重连 ticker 句柄 */
    FTSTicker::FDelegateHandle ReconnectTicker;

    /** 周期事件注册表：Name → ticker 句柄 */
    TMap<FString, FTSTicker::FDelegateHandle> RepeatTickers;
};
