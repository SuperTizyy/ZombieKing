#include "GameBackendSubsystem.h"
#include "GameBackendSettings.h"   // UGameBackendSettings（后端基址/超时/重试/并发/鉴权头配置）
#include "GameBackendEndpoints.h"  // 端点路径契约（BuildRpcPath 使用 RpcPrefix）
#include "HttpModule.h"            // FHttpModule（引擎 HTTP 模块入口：CreateRequest）
#include "Misc/Base64.h"           // FBase64（Basic 鉴权头编码）
#include "Dom/JsonObject.h"        // FJsonObject（RPC 报文解包）
#include "Dom/JsonValue.h"         // FJsonValue（双重编码）
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ZombieKing.h"            // LogZombieKing（传输层日志）

FName UGameBackendSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Backend"));
}

void UGameBackendSubsystem::OnServiceInitialize()
{
    // 启动即复位：队列、在途记录、并发计数与令牌全部清空，保证每次运行从干净状态开始
    PendingQueue.Reset();
    InFlightRequests.Reset();
    ActiveRequestCount = 0;
    NextRequestId = 1;
    AuthToken.Reset();
}

void UGameBackendSubsystem::OnServiceDeinitialize()
{
    // 关闭时清空全部状态。
    // 说明：已发出的 HTTP 请求无法撤回，其回执仍可能到达；HandleRequestComplete 会因
    //       IsServiceReady()==false 而直接丢弃，因此这里只需清理本地状态、无需等待在途请求。
    PendingQueue.Reset();
    InFlightRequests.Reset();
    ActiveRequestCount = 0;
}

void UGameBackendSubsystem::SetAuthToken(const FString& InToken)
{
    // 只保存令牌值；注入请求头的工作统一由 ApplyCommonHeaders 完成，避免两处逻辑分叉
    AuthToken = InToken;
}

void UGameBackendSubsystem::ClearAuthToken()
{
    // 登出场景：清空后所有后续请求都不再携带鉴权头
    AuthToken.Reset();
}

void UGameBackendSubsystem::SendRequest(const FGameBackendRequest& Request)
{
    // 蓝图/Lua 入口：无一对一回调，回执只经全局 OnBackendResponse 广播（传 nullptr 表示"无回调"）
    SendRequestWithCallback(Request, nullptr);
}

void UGameBackendSubsystem::SendRequestWithCallback(const FGameBackendRequest& Request, TFunction<void(const FGameBackendResponse&)> OnComplete)
{
    // 组装内部记录（回调随记录一起走，避免把回调对象塞进引擎委托）
    FGameBackendPendingRequest Item;
    Item.Request = Request;
    Item.OnComplete = MoveTemp(OnComplete);

    // 交给入队逻辑统一处理（含被禁用/参数非法的即时失败路径）
    EnqueueRequest(MoveTemp(Item));
}

void UGameBackendSubsystem::EnqueueRequest(FGameBackendPendingRequest Item)
{
    const UGameBackendSettings* Settings = GetDefault<UGameBackendSettings>();

    // 后端被禁用（纯单机调试）：立即失败返回，避免调用方无限等待
    if (!Settings->bBackendEnabled)
    {
        FailImmediately(Item, TEXT("后端已在配置中禁用（UGameBackendSettings::bBackendEnabled=false）"));
        return;
    }

    // 契约校验：路径必须是"相对路径且以 / 开头"，否则与 BaseUrl 拼接会得到非法 URL。
    // 这里只做边界校验，不猜测调用方意图（是否合法端点由契约头 GameBackendEndpoints 约定）。
    if (Item.Request.Path.IsEmpty() || Item.Request.Path[0] != TEXT('/'))
    {
        FailImmediately(Item, TEXT("请求路径非法：必须是形如 /api/v1/... 的相对路径"));
        return;
    }

    // 入队并尝试立即发送（若并发已满则先排队，待有名额释放时由 PumpQueue 补位）
    PendingQueue.Add(MoveTemp(Item));
    PumpQueue();
}

void UGameBackendSubsystem::PumpQueue()
{
    // 服务已关闭：不再发起新请求（避免关闭后仍产生网络行为）
    if (!IsServiceReady())
    {
        return;
    }

    // 并发上限（配置值至少为 1，防止误配成 0 导致队列永久卡死）
    const int32 MaxConcurrent = FMath::Max(1, GetDefault<UGameBackendSettings>()->MaxConcurrentRequests);

    // 循环补位：队列非空且有空闲名额时持续发出（一次可填补多个名额）
    while (PendingQueue.Num() > 0 && ActiveRequestCount < MaxConcurrent)
    {
        // 取队首（FIFO：保证请求先后顺序符合调用方的业务预期）
        FGameBackendPendingRequest Item = PendingQueue[0];
        PendingQueue.RemoveAt(0);

        StartRequest(MoveTemp(Item));
    }
}

void UGameBackendSubsystem::StartRequest(FGameBackendPendingRequest Item)
{
    const UGameBackendSettings* Settings = GetDefault<UGameBackendSettings>();

    // 创建引擎请求对象（线程安全引用：回执可能由 HTTP 线程投递到游戏线程队列，故必须用 ThreadSafe 版本）
    FHttpRequestRef HttpRequest = FHttpModule::Get().CreateRequest();

    // URL 由"配置基址 + 契约相对路径"拼接；统一在此拼接可避免各处口径不一致
    HttpRequest->SetURL(Settings->BaseUrl + Item.Request.Path);
    HttpRequest->SetVerb(Item.Request.Method);

    // 本项目后端统一 JSON 报文
    HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

    // 超时由配置统一控制（避免单个请求长时间挂起而占用并发名额）
    HttpRequest->SetTimeout(Settings->RequestTimeoutSeconds);

    // 公共头注入（鉴权等）；子类可扩展（见 ApplyCommonHeaders 注释）
    ApplyCommonHeaders(*HttpRequest, Item.Request);

    // 有请求体才写入（GET 类空体请求保持无内容）
    if (!Item.Request.BodyJson.IsEmpty())
    {
        HttpRequest->SetContentAsString(Item.Request.BodyJson);
    }

    // 分配在途 Id 并登记记录：引擎回执只带引擎对象，靠这个 Id 才能找回"我们自己的请求上下文"
    Item.RequestId = NextRequestId++;
    InFlightRequests.Add(Item.RequestId, Item);

    // 占用一个并发名额（回执处理时会释放，并触发 PumpQueue 补位）
    ++ActiveRequestCount;

    // 绑定回执（附加参数 RequestId 会被拼在引擎委托的三个形参之后传给 HandleRequestComplete）
    HttpRequest->OnProcessRequestComplete().BindUObject(this, &UGameBackendSubsystem::HandleRequestComplete, Item.RequestId);

    if (!HttpRequest->ProcessRequest())
    {
        // 引擎直接拒绝发出（平台不支持/句柄耗尽等）：走与"传输失败"完全相同的收尾路径，
        // 保证调用方无论成功失败都一定能拿到回执（绝不出现"发了却没回音"的情况）。
        HandleRequestComplete(HttpRequest, nullptr, false, Item.RequestId);
    }
}

void UGameBackendSubsystem::HandleRequestComplete(FHttpRequestPtr InRequest, FHttpResponsePtr InResponse, bool bProcessedSuccessfully, int32 InRequestId)
{
    // 服务已关闭：丢弃迟到回执（此时不再改动任何状态，也不再广播）
    if (!IsServiceReady())
    {
        return;
    }

    // 取出在途记录；取不到说明已被清理，直接返回（避免重复扣减并发计数而破坏限流）
    FGameBackendPendingRequest Item;
    if (!InFlightRequests.RemoveAndCopyValue(InRequestId, Item))
    {
        return;
    }

    // 释放并发名额（若判定需重试，会重新入队并在下一次发送时再次占用名额）
    --ActiveRequestCount;

    // 归一化回执：把"引擎层结果"翻译成调用方可直接消费的统一结构
    FGameBackendResponse Response;
    Response.RequestTag = Item.Request.RequestTag;
    Response.AttemptCount = Item.AttemptCount + 1;

    bool bShouldRetry = false;

    if (bProcessedSuccessfully && InResponse.IsValid())
    {
        // 传输成功：按 HTTP 状态码判定业务成功（2xx 视为成功，其余统一视为业务失败）
        Response.StatusCode = InResponse->GetResponseCode();
        Response.BodyJson = InResponse->GetContentAsString();
        Response.bSuccess = (Response.StatusCode >= 200 && Response.StatusCode < 300);
        if (!Response.bSuccess)
        {
            Response.ErrorMessage = FString::Printf(TEXT("服务端返回 HTTP %d"), Response.StatusCode);
        }
    }
    else
    {
        // 传输失败（超时/DNS/断网等）：引擎不区分具体原因，这里统一归一为传输失败文本
        Response.bSuccess = false;
        Response.ErrorMessage = TEXT("传输失败（超时或连接错误）");

        // 仅"幂等且显式标记可重试"的请求才重试：非幂等请求（建房/入房）重试会造成重复操作
        const int32 MaxRetries = (Item.Request.MaxRetriesOverride >= 0)
            ? Item.Request.MaxRetriesOverride
            : GetDefault<UGameBackendSettings>()->DefaultMaxRetries;
        bShouldRetry = Item.Request.bRetryable && Item.AttemptCount < MaxRetries;
    }

    if (bShouldRetry)
    {
        // 记录已尝试次数后回到队首：既保持请求相对顺序，又让重试优先于新请求
        FGameBackendPendingRequest RetryItem = MoveTemp(Item);
        RetryItem.AttemptCount += 1;

        // 日志中的"第 N 次尝试"基于递增后的次数计算，须在把记录移入队列之前取好
        const int32 NextAttemptNumber = RetryItem.AttemptCount + 1;

        PendingQueue.Insert(MoveTemp(RetryItem), 0);

        UE_LOG(LogZombieKing, Warning, TEXT("[后端] 请求 %s 传输失败，准备第 %d 次尝试"),
            *Response.RequestTag.ToString(), NextAttemptNumber);

        // 名额已释放，补位发送（重试项此刻位于队首，会被优先发出）
        PumpQueue();
        return;
    }

    // 先回调发起方（如会话服务据回执推进状态机），保证其状态先于全局订阅方更新
    if (Item.OnComplete)
    {
        Item.OnComplete(Response);
    }

    // 再广播全局事件（Lua / UI / 其它服务订阅）
    OnBackendResponse.Broadcast(Response);

    // 补位发送排队中的请求
    PumpQueue();
}

void UGameBackendSubsystem::FailImmediately(FGameBackendPendingRequest& Item, const FString& ErrorMessage)
{
    // 组装统一结构的失败回执（与正常回执同构，调用方无需为"未联网"写第二套处理逻辑）
    FGameBackendResponse Response;
    Response.RequestTag = Item.Request.RequestTag;
    Response.bSuccess = false;
    Response.ErrorMessage = ErrorMessage;

    // 记录一条日志：这类失败通常是配置或调用方式问题，值得在日志里显式可见
    UE_LOG(LogZombieKing, Warning, TEXT("[后端] 请求 %s 未发出：%s"),
        *Response.RequestTag.ToString(), *ErrorMessage);

    if (Item.OnComplete)
    {
        Item.OnComplete(Response);
    }

    OnBackendResponse.Broadcast(Response);
}

void UGameBackendSubsystem::ApplyCommonHeaders(IHttpRequest& Request, const FGameBackendRequest& Item)
{
    const UGameBackendSettings* Settings = GetDefault<UGameBackendSettings>();

    switch (Item.Auth)
    {
    case EGameBackendAuth::BasicKey:
        // 登录端点：Basic 鉴权（用户名=ServerKey，密码空）——Nakama REST 网关口径
        if (!Settings->ServerKey.IsEmpty())
        {
            const FString Creds = FBase64::Encode(Settings->ServerKey + TEXT(":"));
            Request.SetHeader(Settings->AuthHeaderName, TEXT("Basic ") + Creds);
        }
        break;
    case EGameBackendAuth::Bearer:
        // 已登录/已拿到令牌时注入鉴权头；字段名与前缀来自配置（便于对齐后端约定，无需改代码）
        if (!AuthToken.IsEmpty())
        {
            Request.SetHeader(Settings->AuthHeaderName, Settings->AuthHeaderPrefix + AuthToken);
        }
        break;
    case EGameBackendAuth::None:
    default:
        break;
    }

    // 扩展提示：反作弊请求签名、客户端版本、渠道号、灰度标记等公共头应在子类覆写本函数处追加，
    //           以保证"所有出站流量都在同一处加签"，这是把 HTTP 出口收敛到本服务的核心收益。
}

// ---------------------------------------------------------------------------
// Nakama RPC 报文机制
// ---------------------------------------------------------------------------

FString UGameBackendSubsystem::EncodeRpcPayload(const FString& BusinessJson)
{
    // 双重编码：业务 JSON 作为字符串值再转义一次（Nakama REST RPC 的 body 约定）。
    // 业务 JSON 由 FJsonWriter 紧凑输出（单行、无控制字符），只需转义反斜杠与引号。
    const FString& Src = BusinessJson.IsEmpty() ? FString(TEXT("{}")) : BusinessJson;
    const FString Escaped = Src.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
    return TEXT("\"") + Escaped + TEXT("\"");
}

FString UGameBackendSubsystem::DecodeRpcPayload(const FString& ResponseBodyJson)
{
    if (ResponseBodyJson.IsEmpty())
    {
        return FString();
    }
    TSharedPtr<FJsonObject> Object;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBodyJson);
    if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
    {
        FString Payload;
        if (Object->TryGetStringField(TEXT("payload"), Payload))
        {
            return Payload; // RPC 回执：解出业务 JSON 字符串
        }
    }
    return ResponseBodyJson; // 非 RPC 回执（如登录）原样返回
}

FString UGameBackendSubsystem::BuildRpcPath(const FString& RpcId)
{
    return FString(GameBackendEndpoints::RpcPrefix) + RpcId;
}
