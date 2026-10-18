#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"     // UGameServiceSubsystem（GameInstance 层服务统一基类）
#include "GameSessionSubsystem.generated.h"

class UGameBackendSubsystem;          // 后端传输网关（会话请求统一经它发出）
struct FGameBackendResponse;          // 后端回执（仅作回调形参，头文件无需完整类型）

/**
 * 会话/房间的会话状态机（生命周期阶段）。
 * 说明：刻意只用与"具体后端协议无关"的通用阶段，便于后端演进时无需改动枚举。
 */
UENUM(BlueprintType)
enum class EGameSessionState : uint8
{
    /** 空闲：尚未创建、也未加入任何会话 */
    Idle      UMETA(DisplayName = "空闲"),
    /** 建立中：已向后端发起创建/加入请求，等待回执 */
    Connecting UMETA(DisplayName = "建立中"),
    /** 会话内：后端已确认进入会话 */
    InSession UMETA(DisplayName = "会话内"),
    /** 退出中：已发起离开请求，等待后端回执 */
    Leaving   UMETA(DisplayName = "退出中")
};

/** 会话状态变化广播（供 UI / Lua 表现层订阅：加载界面显隐、房间列表刷新、错误提示等） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnGameSessionStateChanged, EGameSessionState, NewState, EGameSessionState, OldState);

/**
 * 竞技生化网游：会话 / 房间服务（GameInstance 层服务）
 *
 * 后端映射（Nakama TS 权威模块，见 ZombieKingServer/nakama/modules/src/rpc/room.ts）：
 *  - 会话 = 房间（game.room 权威成员表）：SessionId 即 room_id；MatchId 即服务端下发的
 *    zk_lobby 实时房间 Match 标识（"uuid.<node>" 格式，供 WS match_join 与房间聊天）；
 *  - 创建/加入/离开/开局 = rpc_room_create / rpc_room_join / rpc_room_leave / rpc_room_start；
 *  - 断线重连不设令牌：房间席位以服务端成员表为准，重连后凭 room_id 重新 join 即可恢复席位。
 *
 * 职责边界（严格遵循"机制在 C++、规则在 Lua"的热更新纪律）：
 *  - 本服务只做**机制**：把会话请求交给后端传输网关（UGameBackendSubsystem）、
 *    解析契约响应、推进会话状态机、维护会话上下文（房间标识 / 实时 Match 标识）；
 *  - "何时建房、几人房、失败怎么提示、要不要自动重连"等**规则**由 Lua 规则脚本决定（可热更）；
 *  - 本服务不维护玩家名单：对局内名单以 GameState/GameMode（PostLogin/Logout）为唯一口径。
 *
 * 与后端的关系：
 *  - 所有出站流量统一走 UGameBackendSubsystem（唯一 HTTP 出口），本服务不直接调引擎 HTTP；
 *  - 端点与 RPC id 属协议契约，登记在 GameBackendEndpoints（改动须前后端同版本对齐）。
 *
 * 生命周期：随 GameInstance 存活（跨关卡不重建），因此"换图后仍知道自己属于哪个会话"。
 */
UCLASS()
class ZOMBIEKING_API UGameSessionSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 只读查询（BlueprintPure：供 C++ / 蓝图 / Lua 取值）
    // -------------------------------------------------------------------------
    /** 当前会话状态 */
    UFUNCTION(BlueprintPure, Category = "Session")
    EGameSessionState GetSessionState() const { return SessionState; }

    /** 当前会话（房间）标识（未在会话内时为空串） */
    UFUNCTION(BlueprintPure, Category = "Session")
    FString GetSessionId() const { return SessionId; }

    /** 当前会话的实时 Match 标识（供 WS match_join 加入房间聊天；未在会话内时为空串） */
    UFUNCTION(BlueprintPure, Category = "Session")
    FString GetMatchId() const { return MatchId; }

    /** 会话人数上限（由创建请求确定；加入他人会话时取回执 members 对应的上限，未知为 0） */
    UFUNCTION(BlueprintPure, Category = "Session")
    int32 GetMaxPlayers() const { return MaxPlayers; }

    /** 是否已在会话内（表现层判断"能否开始对局"的常用条件） */
    UFUNCTION(BlueprintPure, Category = "Session")
    bool IsInSession() const { return SessionState == EGameSessionState::InSession; }

    /** 最近一次会话操作失败原因（成功时为空串；UI/Lua 可据此提示玩家） */
    UFUNCTION(BlueprintPure, Category = "Session")
    FString GetLastError() const { return LastError; }

    // -------------------------------------------------------------------------
    // 状态变化事件（BlueprintAssignable：Lua / UI 订阅）
    // -------------------------------------------------------------------------
    /** 会话状态变化事件（仅在状态真正发生迁移时广播一次） */
    UPROPERTY(BlueprintAssignable, Category = "Session")
    FOnGameSessionStateChanged OnSessionStateChanged;

    // -------------------------------------------------------------------------
    // 会话机制接口（BlueprintCallable：供 Lua 规则脚本驱动；本类不做规则判断）
    // -------------------------------------------------------------------------
    /**
     * 请求创建会话（rpc_room_create）。
     * @param Mode           对局模式（"tdm_5v5" / "zombie_infection"，仅作非空校验，合法性由服务端裁决）
     * @param InMaxPlayers   人数上限（必须 > 0，仅作边界校验）
     * @return 是否已受理并发往后端（false = 参数非法、状态不允许或后端服务不可用）
     */
    UFUNCTION(BlueprintCallable, Category = "Session")
    bool RequestCreateSession(const FString& Mode, int32 InMaxPlayers);

    /**
     * 请求加入指定会话（rpc_room_join；知道 room_id 即视为受邀）。
     * @param InSessionId 目标房间标识（不可为空）
     * @return 是否已受理
     */
    UFUNCTION(BlueprintCallable, Category = "Session")
    bool RequestJoinSession(const FString& InSessionId);

    /**
     * 请求离开当前会话（rpc_room_leave；房主离开=解散）。
     * 说明：即使后端离开失败，本地也会收回会话上下文并回到空闲态——
     *       否则玩家会卡在"退出中"而无法进行任何后续操作。
     * @return 是否已受理
     */
    UFUNCTION(BlueprintCallable, Category = "Session")
    bool RequestLeaveSession();

    /**
     * 请求开局（rpc_room_start；服务端校验仅房主可发起，成员随 party 整队进入撮合队列）。
     * 成功后本地立即收回会话上下文（房间已解散，进入对局流程由撮合广播/轮询驱动）。
     * @return 是否已受理
     */
    UFUNCTION(BlueprintCallable, Category = "Session")
    bool RequestStartSession();

    /**
     * 请求会话列表（rpc_room_list）：结果经 OnBackendResponse 全局广播（标签 Session.List），
     * 由 Lua 规则层解析展示（本服务不缓存列表，避免与后端权威不一致）。
     * @return 是否已发出
     */
    UFUNCTION(BlueprintCallable, Category = "Session")
    bool RequestListSessions();

protected:
    // -------------------------------------------------------------------------
    // 服务生命周期
    // -------------------------------------------------------------------------
    /** 启动：回到干净的空闲态（防止编辑器内 PIE 复用同一 GameInstance 时残留上一局的会话信息） */
    virtual void OnServiceInitialize() override;

    /** 关闭：清空会话上下文（内存态数据，无需外部资源释放） */
    virtual void OnServiceDeinitialize() override;

    /**
     * 统一的会话状态迁移入口（唯一写 SessionState 的地方）。
     * 目的：保证"改状态"与"广播事件"永远成对发生，且状态相同则不重复广播（幂等）。
     */
    void SetSessionState(EGameSessionState NewState);

    // -------------------------------------------------------------------------
    // 后端回执处理（按请求标签分派；在游戏线程执行——引擎 HTTP 回执默认在游戏线程投递）
    // -------------------------------------------------------------------------
    void HandleCreateSessionResponse(const FGameBackendResponse& InResponse);
    void HandleJoinSessionResponse(const FGameBackendResponse& InResponse);
    void HandleLeaveSessionResponse(const FGameBackendResponse& InResponse);
    void HandleStartSessionResponse(const FGameBackendResponse& InResponse);

private:
    /** 取后端传输网关（不可用时返回 nullptr；调用方须自行处理不可用分支） */
    UGameBackendSubsystem* GetBackend() const;

    /** 发送一个 RPC 风格会话请求（内部统一做双重编码与回调绑定） */
    bool SendRoomRpc(const FString& RpcId, const FString& BusinessJson, FName Tag, bool bRetryable,
                     TFunction<void(const FGameBackendResponse&)> OnComplete);

    /** 清空会话上下文（房间标识 / 实时 Match 标识 / 人数上限） */
    void ResetSessionContext();

private:
    /** 当前会话状态（仅经 SetSessionState 修改） */
    EGameSessionState SessionState = EGameSessionState::Idle;

    /** 当前会话（房间）标识（由后端在创建/加入的回执中下发） */
    FString SessionId;

    /** 实时房间 Match 标识（"uuid.<node>" 格式，供 WS match_join） */
    FString MatchId;

    /** 会话人数上限（由创建请求参数确定） */
    int32 MaxPlayers = 0;

    /** 最近一次失败原因（仅作展示与排障，不参与任何规则判断） */
    FString LastError;
};
