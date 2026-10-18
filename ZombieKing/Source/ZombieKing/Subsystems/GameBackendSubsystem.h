#pragma once

#include "CoreMinimal.h"
#include "GameServiceSubsystem.h"        // UGameServiceSubsystem（GameInstance 层服务统一基类）
// 引擎 HTTP 接口：回执处理签名里用到 FHttpRequestPtr / FHttpResponsePtr，且扩展点需要 IHttpRequest&，
// 因此这里直接包含而非前向声明（FHttpRequestPtr 是 TSharedPtr 别名，前向声明无法使用）
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GameBackendSubsystem.generated.h"

/**
 * 鉴权模式（Nakama REST 两种鉴权头并存：登录端点用 ServerKey Basic，其余用会话 Bearer）。
 */
UENUM(BlueprintType)
enum class EGameBackendAuth : uint8
{
    /** 携带会话令牌（默认；未登录时令牌为空则裸发，由服务端拒绝） */
    Bearer    UMETA(DisplayName = "会话令牌"),
    /** 携带 Basic（用户名=配置 ServerKey，密码空）：仅登录端点使用 */
    BasicKey  UMETA(DisplayName = "服务器密钥"),
    /** 不带任何鉴权头 */
    None      UMETA(DisplayName = "无鉴权")
};

/**
 * 后端请求（**机制层**通用结构：不含任何业务端点语义）。
 * 设计意图：传输层不认识"会话/匹配/账号"，只认"往哪发、发什么、回执挂什么标签"，
 *           因此新增业务端点无需改动传输层（端点由调用方经 GameBackendEndpoints 指定）。
 */
USTRUCT(BlueprintType)
struct FGameBackendRequest
{
    GENERATED_BODY()

    /** HTTP 方法（默认 POST：本项目后端统一 RPC 风格 POST + JSON 体） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FString Method = TEXT("POST");

    /** 相对路径（与配置 BaseUrl 拼接），取值见 GameBackendEndpoints */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FString Path;

    /** 请求体（JSON 字符串；无体时留空。Nakama RPC 需先经 EncodeRpcPayload 双重编码） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FString BodyJson;

    /** 鉴权模式（见 EGameBackendAuth；登录端点用 BasicKey，RPC 用默认 Bearer） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    EGameBackendAuth Auth = EGameBackendAuth::Bearer;

    /** 调用方自定义标签：回执经它与发起方匹配（取值见 GameBackendRequestTags） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FName RequestTag;

    /**
     * 是否允许重试（默认 false）。
     * 纪律：**只有幂等请求**才可置 true。创建会话/加入会话这类非幂等操作若重试，
     *       在"请求已送达但回执丢失"的网络场景下会造成重复建房/重复入房。
     */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    bool bRetryable = false;

    /** 覆盖配置中的最大重试次数（-1 表示使用 UGameBackendSettings::DefaultMaxRetries） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    int32 MaxRetriesOverride = -1;
};

/** 后端回执（**机制层**通用结构：成功与否、状态码、原始体、归一化错误文本） */
USTRUCT(BlueprintType)
struct FGameBackendResponse
{
    GENERATED_BODY()

    /** 与请求一一对应的标签（用于发起方分派） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FName RequestTag;

    /** 是否业务成功（传输成功且 HTTP 2xx） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    bool bSuccess = false;

    /** HTTP 状态码（传输层失败时为 0） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    int32 StatusCode = 0;

    /** 响应体原文（JSON 字符串；调用方自行按契约解析） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FString BodyJson;

    /** 本次实际尝试次数（1 = 一次成功；>1 表示经历过重试） */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    int32 AttemptCount = 0;

    /** 失败原因的归一化描述（传输失败文本 / "服务端返回 HTTP 500" / "后端已禁用"等），供 UI 与 Lua 展示 */
    UPROPERTY(BlueprintReadWrite, Category = "Backend")
    FString ErrorMessage;
};

/** 后端回执广播（供 Lua 规则脚本 / UI / 其它服务订阅） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnGameBackendResponse, const FGameBackendResponse&, Response);

/**
 * 传输层"待发/在途"记录（**纯 C++ 内部结构**，刻意不做成 USTRUCT：
 * 它含有 TFunction 回调对象，不需要也不应该暴露给反射系统）。
 */
struct FGameBackendPendingRequest
{
    /** 原始请求内容 */
    FGameBackendRequest Request;

    /** 已尝试次数（0 = 尚未发出） */
    int32 AttemptCount = 0;

    /** 在途请求 Id（由传输层分配，用于把引擎 HTTP 回执映射回本记录） */
    int32 RequestId = INDEX_NONE;

    /** 发起方完成回调（可为空：Lua 侧发起的请求只靠全局 OnBackendResponse 接收回执） */
    TFunction<void(const FGameBackendResponse&)> OnComplete;
};

/**
 * 竞技生化网游：自研后端**传输网关**（GameInstance 层服务）
 *
 * 一句话定位：全项目所有出站 HTTP 流量的**唯一出口**。
 *
 * 为什么要"唯一出口"（而非各业务各自调 HTTP）：
 *  - 鉴权头、客户端版本、反作弊签名等公共头只需在一处注入（见 ApplyCommonHeaders 扩展点），
 *    不会随业务代码扩散而漏加；
 *  - 超时、重试、并发上限、错误归一化等策略集中管理，行为一致、可统一观测与调参；
 *  - 业务方（C++ 的会话服务、Lua 规则脚本）只关心"发什么、拿到什么"，与传输细节解耦。
 *
 * 职责边界（严格遵循热更新纪律）：
 *  - C++ 只做**机制**：请求编排、超时重试、并发限流、公共头注入、回执归一化；
 *  - 端点路径与报文字段属于协议契约（GameBackendEndpoints），不是可调规则；
 *  - "何时建会话、几人房、失败怎么提示"等**规则**一律在 Lua（可热更）。
 *
 * 线程约定（已核实引擎源码）：`FHttpManager` 默认请求完成委托策略为 `CompleteOnGameThread`，
 * 回执在游戏线程触发，故本类可安全直接读写 UObject 状态与广播委托。
 *
 * TODO（后续里程碑）：
 *  - 实时推送（房间状态变更、匹配进度）需要长连接，将另加 WebSocket 通道（属独立工作项）；
 *  - 请求签名 / 反作弊加签在 ApplyCommonHeaders 扩展点上追加即可，无需改动本类。
 */
UCLASS()
class ZOMBIEKING_API UGameBackendSubsystem : public UGameServiceSubsystem
{
    GENERATED_BODY()

public:
    /** 服务短名（日志前缀 / 按名定位用） */
    virtual FName GetServiceName() const override;

    // -------------------------------------------------------------------------
    // 发送请求
    // -------------------------------------------------------------------------
    /**
     * 发送请求（蓝图 / Lua 版本）：结果通过全局 OnBackendResponse 委托返回（按 RequestTag 分派）。
     * @param Request 请求内容（路径取值见 GameBackendEndpoints）
     */
    UFUNCTION(BlueprintCallable, Category = "Backend")
    void SendRequest(const FGameBackendRequest& Request);

    /**
     * 发送请求（C++ 版本）：除全局广播外，额外通过 OnComplete 回调把回执直接交给发起方。
     * 之所以区分两个入口：UFUNCTION 不能携带 TFunction 参数，而 C++ 调用方（如会话服务）
     * 需要"请求—回执"一对一绑定，用回调比按标签全局分派更直接可靠。
     * @param Request    请求内容
     * @param OnComplete 完成回调（可为空；为空时等价于 SendRequest）
     */
    void SendRequestWithCallback(const FGameBackendRequest& Request, TFunction<void(const FGameBackendResponse&)> OnComplete);

    // -----------------------------------------------------------------
    // 鉴权令牌（本轮只做注入机制，账号体系另行立项）
    // -----------------------------------------------------------------
    /** 设置鉴权令牌：之后所有出站请求自动带鉴权头（字段名与前缀来自配置） */
    UFUNCTION(BlueprintCallable, Category = "Backend|Auth")
    void SetAuthToken(const FString& InToken);

    /** 清除鉴权令牌（如登出后），之后请求不再带鉴权头 */
    UFUNCTION(BlueprintCallable, Category = "Backend|Auth")
    void ClearAuthToken();

    /** 当前是否已设置鉴权令牌（不返回令牌本身，避免敏感值进日志/UI） */
    UFUNCTION(BlueprintPure, Category = "Backend|Auth")
    bool HasAuthToken() const { return !AuthToken.IsEmpty(); }

    // -------------------------------------------------------------------------
    // Nakama RPC 报文机制（静态 helper：C++ 与 Lua 共用，避免各处手写双重编码出错）
    // -------------------------------------------------------------------------
    /**
     * RPC 请求体双重编码：业务 JSON 字符串 → JSON 字符串字面量（Nakama REST RPC 的 body 约定）。
     * 例：{"a":1} → "\"{\\\"a\\\":1}\""
     */
    UFUNCTION(BlueprintCallable, Category = "Backend|Rpc")
    static FString EncodeRpcPayload(const FString& BusinessJson);

    /**
     * RPC 响应解包：{"payload":"<业务JSON字符串>"} → 业务 JSON 字符串；
     * 无 payload 字段时原样返回（登录等非 RPC 回执）。
     */
    UFUNCTION(BlueprintCallable, Category = "Backend|Rpc")
    static FString DecodeRpcPayload(const FString& ResponseBodyJson);

    /** 拼 RPC 路径：/v2/rpc/<rpc_id>（rpc_id 取值见 GameBackendRpcIds） */
    UFUNCTION(BlueprintCallable, Category = "Backend|Rpc")
    static FString BuildRpcPath(const FString& RpcId);

    // -------------------------------------------------------------------------
    // 观测
    // -------------------------------------------------------------------------
    /** 当前未完成的请求数（排队中 + 在途） */
    UFUNCTION(BlueprintPure, Category = "Backend")
    int32 GetOutstandingRequestCount() const { return PendingQueue.Num() + ActiveRequestCount; }

    /** 后端回执事件（BlueprintAssignable：Lua / UI / 其它服务订阅） */
    UPROPERTY(BlueprintAssignable, Category = "Backend")
    FOnGameBackendResponse OnBackendResponse;

protected:
    // -------------------------------------------------------------------------
    // 服务生命周期
    // -------------------------------------------------------------------------
    /** 启动：清空队列与令牌，回到干净状态 */
    virtual void OnServiceInitialize() override;

    /** 关闭：清空队列与在途记录（迟到回执会因 IsServiceReady()==false 被安全丢弃） */
    virtual void OnServiceDeinitialize() override;

    // -------------------------------------------------------------------------
    // 扩展点（子类覆写；当前仅注入鉴权头）
    // -------------------------------------------------------------------------
    /**
     * 公共请求头注入点（所有出站请求都会经过这里）。
     * 用途：按请求声明的鉴权模式注入 Bearer/Basic 头；后续"反作弊请求签名、客户端版本号、
     *       渠道号、灰度标记"等公共头也在此追加，
     *       从而保证公共头永远只在一个地方维护（这正是"唯一出口"的价值所在）。
     */
    virtual void ApplyCommonHeaders(IHttpRequest& Request, const FGameBackendRequest& Item);

private:
    /** 入队并尝试发送（含"后端被禁用/参数非法"的即时失败处理） */
    void EnqueueRequest(FGameBackendPendingRequest Item);

    /** 用空闲并发名额补位发送排队请求 */
    void PumpQueue();

    /** 真正发出一个请求（拼 URL、注入公共头、登记在途记录、绑定回执） */
    void StartRequest(FGameBackendPendingRequest Item);

    /**
     * 引擎 HTTP 回执处理（游戏线程）。
     * @param InRequest              引擎请求对象（未使用，签名由委托固定）
     * @param InResponse             引擎响应对象（传输失败时为空）
     * @param bProcessedSuccessfully 引擎是否成功完成本次传输
     * @param InRequestId            在途请求 Id（由绑定时的附加参数传入，用于找回记录）
     */
    void HandleRequestComplete(FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bProcessedSuccessfully, int32 InRequestId);

    /** 立即以指定错误结束一个请求（用于"后端被禁用/参数非法"等无需联网的情形），保证调用方一定拿到回执 */
    void FailImmediately(FGameBackendPendingRequest& Item, const FString& ErrorMessage);

private:
    /** 待发队列（FIFO，保证请求先后顺序；重试的请求会回到队首） */
    TArray<FGameBackendPendingRequest> PendingQueue;

    /** 在途请求：请求 Id → 记录（回执靠 Id 找回，避免把回调对象塞进引擎委托） */
    TMap<int32, FGameBackendPendingRequest> InFlightRequests;

    /** 当前在途请求数（受配置 MaxConcurrentRequests 限制） */
    int32 ActiveRequestCount = 0;

    /** 请求 Id 分配器（从 1 开始；0 与 INDEX_NONE 保留为"无效"） */
    int32 NextRequestId = 1;

    /** 鉴权令牌（运行时注入；空表示未登录/无鉴权） */
    FString AuthToken;
};
