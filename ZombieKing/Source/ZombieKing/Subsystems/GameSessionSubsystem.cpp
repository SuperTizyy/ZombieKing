#include "GameSessionSubsystem.h"
#include "GameBackendSubsystem.h"     // UGameBackendSubsystem（HTTP 唯一出口：会话请求经它发出）
#include "GameBackendEndpoints.h"     // 端点路径 / RPC id / 请求标签（前后端协议契约）
#include "Engine/GameInstance.h"      // UGameInstance::GetSubsystem（取后端服务）
#include "Dom/JsonObject.h"           // FJsonObject（组装/解析 JSON 报文）
#include "Serialization/JsonReader.h"  // TJsonReaderFactory
#include "Serialization/JsonSerializer.h" // FJsonSerializer
#include "Serialization/JsonWriter.h"  // TJsonWriterFactory
#include "ZombieKing.h"               // LogZombieKing（会话流程日志）

namespace
{
    /**
     * 把 JSON 对象序列化为字符串。
     * 放在匿名命名空间：仅本文件使用，保持内部链接，避免污染全局符号。
     */
    FString SerializeJsonObject(const TSharedRef<FJsonObject>& InObject)
    {
        FString OutString;
        const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutString);
        FJsonSerializer::Serialize(InObject, Writer);
        return OutString;
    }

    /**
     * 解析响应体为 JSON 对象；解析失败（空体或非法 JSON）返回空指针，
     * 调用方一律按"服务端响应不符合契约"处理，避免用空对象继续往下跑。
     */
    TSharedPtr<FJsonObject> DeserializeJsonObject(const FString& InJson)
    {
        if (InJson.IsEmpty())
        {
            return nullptr;
        }

        TSharedPtr<FJsonObject> Object;
        const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InJson);
        if (FJsonSerializer::Deserialize(Reader, Object) && Object.IsValid())
        {
            return Object;
        }
        return nullptr;
    }
}

FName UGameSessionSubsystem::GetServiceName() const
{
    // 稳定短名：日志前缀与按名定位均依赖它，一经发布不得随意变更
    return FName(TEXT("Session"));
}

UGameBackendSubsystem* UGameSessionSubsystem::GetBackend() const
{
    // 后端是 GameInstance 层服务，经 GameInstance 取用（不用缓存，避免与服务重建/销毁时序耦合）
    UGameInstance* GameInstance = GetGameInstance();
    return GameInstance ? GameInstance->GetSubsystem<UGameBackendSubsystem>() : nullptr;
}

void UGameSessionSubsystem::OnServiceInitialize()
{
    // 复位为空闲态：GameInstance 在编辑器内可能被复用，必须清掉上一次运行残留的会话上下文
    SessionState = EGameSessionState::Idle;
    ResetSessionContext();
    LastError.Reset();
}

void UGameSessionSubsystem::OnServiceDeinitialize()
{
    // 会话上下文都是内存态数据，无需释放外部资源；这里只清空，避免关闭后仍被外部读取
    ResetSessionContext();
}

void UGameSessionSubsystem::ResetSessionContext()
{
    // 三个字段构成"会话上下文"：清空即代表当前不属于任何会话
    SessionId.Reset();
    MatchId.Reset();
    MaxPlayers = 0;
}

bool UGameSessionSubsystem::SendRoomRpc(const FString& RpcId, const FString& BusinessJson, FName Tag, bool bRetryable,
                                        TFunction<void(const FGameBackendResponse&)> OnComplete)
{
    UGameBackendSubsystem* Backend = GetBackend();
    if (!Backend)
    {
        LastError = TEXT("后端传输服务不可用");
        UE_LOG(LogZombieKing, Error, TEXT("[会话] %s 失败：%s"), *RpcId, *LastError);
        return false;
    }

    FGameBackendRequest Request;
    Request.Path = UGameBackendSubsystem::BuildRpcPath(RpcId);
    // Nakama REST RPC 约定：body 为业务 JSON 的双重编码（机制层统一处理，调用方只给业务 JSON）
    Request.BodyJson = UGameBackendSubsystem::EncodeRpcPayload(BusinessJson);
    Request.RequestTag = Tag;
    Request.bRetryable = bRetryable;

    Backend->SendRequestWithCallback(Request, MoveTemp(OnComplete));
    return true;
}

bool UGameSessionSubsystem::RequestCreateSession(const FString& Mode, int32 InMaxPlayers)
{
    // 边界校验（只做"输入是否合法"；"允不允许建房"属规则，交给 Lua 判定）
    if (Mode.IsEmpty() || InMaxPlayers <= 0)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 创建请求参数非法（模式为空或人数上限<=0），已拒绝"));
        return false;
    }

    // 状态前置校验：只有空闲态才允许发起；这同时天然防止同一时刻并发多个会话请求
    if (SessionState != EGameSessionState::Idle)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 当前状态非空闲，拒绝创建请求"));
        return false;
    }

    LastError.Reset();

    // 进入"建立中"：广播状态变化，表现层据此显示创建中界面
    SetSessionState(EGameSessionState::Connecting);

    // 组装 RPC 业务体（字段名即前后端契约，见服务端 rpc_room_create；数值显式转 double 以匹配 JSON 数值类型）
    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("mode"), Mode);
    Body->SetNumberField(TEXT("max_members"), static_cast<double>(InMaxPlayers));

    // 建会话非幂等：明确禁止自动重试——若请求已送达而回执丢失，重试会造成重复建房
    const TWeakObjectPtr<UGameSessionSubsystem> WeakThis(this);
    if (!SendRoomRpc(GameBackendRpcIds::RoomCreate, SerializeJsonObject(Body),
        FName(GameBackendRequestTags::SessionCreate), false,
        [WeakThis](const FGameBackendResponse& InResponse)
        {
            if (UGameSessionSubsystem* Self = WeakThis.Get())
            {
                Self->HandleCreateSessionResponse(InResponse);
            }
        }))
    {
        SetSessionState(EGameSessionState::Idle);
        return false;
    }

    // 人数上限只有客户端知道（join 回执不下发容量），故本地先留存，供 UI 显示房间容量
    MaxPlayers = InMaxPlayers;
    return true;
}

bool UGameSessionSubsystem::RequestJoinSession(const FString& InSessionId)
{
    // 边界校验：目标房间标识必须有效
    if (InSessionId.IsEmpty())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 加入请求参数非法（房间标识为空），已拒绝"));
        return false;
    }

    // 状态前置校验：只有空闲态才允许加入（避免"已在会话中还去加入别的会话"）
    if (SessionState != EGameSessionState::Idle)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 当前状态非空闲，拒绝加入请求"));
        return false;
    }

    LastError.Reset();
    SetSessionState(EGameSessionState::Connecting);

    const TSharedRef<FJsonObject> Body = MakeShared<FJsonObject>();
    Body->SetStringField(TEXT("room_id"), InSessionId);

    // 入房非幂等：禁止自动重试（回执丢失时重试会造成重复入房/占用两个席位）
    const TWeakObjectPtr<UGameSessionSubsystem> WeakThis(this);
    if (!SendRoomRpc(GameBackendRpcIds::RoomJoin, SerializeJsonObject(Body),
        FName(GameBackendRequestTags::SessionJoin), false,
        [WeakThis](const FGameBackendResponse& InResponse)
        {
            if (UGameSessionSubsystem* Self = WeakThis.Get())
            {
                Self->HandleJoinSessionResponse(InResponse);
            }
        }))
    {
        SetSessionState(EGameSessionState::Idle);
        return false;
    }

    // 加入他人会话时本地不知道房间容量（join 回执不下发 max_members），故置 0 表示"未知"
    MaxPlayers = 0;
    return true;
}

bool UGameSessionSubsystem::RequestLeaveSession()
{
    // 状态前置校验：不在会话中则无需离开（直接拒绝，避免产生无意义的 Connecting/Idle 抖动）
    if (SessionState == EGameSessionState::Idle)
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 当前不在会话中，忽略离开请求"));
        return false;
    }

    // 处于"建立中"（尚未取得房间标识）时本地直接收回：此时后端还不知道我们是谁，无需发起离开
    if (SessionState != EGameSessionState::InSession || SessionId.IsEmpty())
    {
        ResetSessionContext();
        SetSessionState(EGameSessionState::Idle);
        return true;
    }

    LastError.Reset();
    SetSessionState(EGameSessionState::Leaving);

    // 离开是幂等的（服务端成员表删除，重复离开无副作用），允许传输层有限重试
    const TWeakObjectPtr<UGameSessionSubsystem> WeakThis(this);
    if (!SendRoomRpc(GameBackendRpcIds::RoomLeave, TEXT("{}"),
        FName(GameBackendRequestTags::SessionLeave), true,
        [WeakThis](const FGameBackendResponse& InResponse)
        {
            if (UGameSessionSubsystem* Self = WeakThis.Get())
            {
                Self->HandleLeaveSessionResponse(InResponse);
            }
        }))
    {
        // 后端不可用时也必须让玩家能退出：本地收回上下文，避免卡在"退出中"
        ResetSessionContext();
        SetSessionState(EGameSessionState::Idle);
        return false;
    }

    return true;
}

bool UGameSessionSubsystem::RequestStartSession()
{
    if (SessionState != EGameSessionState::InSession || SessionId.IsEmpty())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 不在会话中，拒绝开局请求"));
        return false;
    }

    LastError.Reset();

    // 开局非幂等（服务端写队列+删房）：禁止自动重试；房主校验由服务端权威执行
    const TWeakObjectPtr<UGameSessionSubsystem> WeakThis(this);
    if (!SendRoomRpc(GameBackendRpcIds::RoomStart, TEXT("{}"),
        FName(GameBackendRequestTags::SessionStart), false,
        [WeakThis](const FGameBackendResponse& InResponse)
        {
            if (UGameSessionSubsystem* Self = WeakThis.Get())
            {
                Self->HandleStartSessionResponse(InResponse);
            }
        }))
    {
        return false;
    }

    return true;
}

bool UGameSessionSubsystem::RequestListSessions()
{
    // 列表为只读查询：幂等，允许重试；结果交 Lua 规则层按标签 Session.List 分派解析
    return SendRoomRpc(GameBackendRpcIds::RoomList, TEXT("{}"),
        FName(GameBackendRequestTags::SessionList), true, nullptr);
}

void UGameSessionSubsystem::HandleCreateSessionResponse(const FGameBackendResponse& InResponse)
{
    // 传输/业务失败：回到空闲态并记录错误文本（UI/Lua 可据 LastError 提示玩家）
    if (!InResponse.bSuccess)
    {
        LastError = InResponse.ErrorMessage;
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 创建失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    // RPC 回执解包（{"payload":"<业务JSON>"} → 业务 JSON）后按契约解析
    const TSharedPtr<FJsonObject> Body = DeserializeJsonObject(UGameBackendSubsystem::DecodeRpcPayload(InResponse.BodyJson));
    if (!Body.IsValid())
    {
        LastError = TEXT("服务端响应不符合契约（非法 JSON）");
        UE_LOG(LogZombieKing, Error, TEXT("[会话] 创建失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    FString NewRoomId;
    if (!Body->TryGetStringField(TEXT("room_id"), NewRoomId) || NewRoomId.IsEmpty())
    {
        // 服务端错误以 200 + {"err":"..."} 或异常码返回时走此分支
        Body->TryGetStringField(TEXT("err"), LastError);
        if (LastError.IsEmpty())
        {
            LastError = TEXT("服务端响应缺少 room_id 字段");
        }
        UE_LOG(LogZombieKing, Error, TEXT("[会话] 创建失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    SessionId = NewRoomId;
    Body->TryGetStringField(TEXT("match_id"), MatchId);

    SetSessionState(EGameSessionState::InSession);
    UE_LOG(LogZombieKing, Log, TEXT("[会话] 已建房 %s（实时 Match %s，人数上限 %d）"),
        *SessionId, MatchId.IsEmpty() ? TEXT("无") : *MatchId, MaxPlayers);
}

void UGameSessionSubsystem::HandleJoinSessionResponse(const FGameBackendResponse& InResponse)
{
    if (!InResponse.bSuccess)
    {
        LastError = InResponse.ErrorMessage;
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 加入失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    const TSharedPtr<FJsonObject> Body = DeserializeJsonObject(UGameBackendSubsystem::DecodeRpcPayload(InResponse.BodyJson));
    if (!Body.IsValid())
    {
        LastError = TEXT("服务端响应不符合契约（非法 JSON）");
        UE_LOG(LogZombieKing, Error, TEXT("[会话] 加入失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    FString NewRoomId;
    if (!Body->TryGetStringField(TEXT("room_id"), NewRoomId) || NewRoomId.IsEmpty())
    {
        Body->TryGetStringField(TEXT("err"), LastError);
        if (LastError.IsEmpty())
        {
            LastError = TEXT("服务端响应缺少 room_id 字段");
        }
        UE_LOG(LogZombieKing, Error, TEXT("[会话] 加入失败：%s"), *LastError);
        SetSessionState(EGameSessionState::Idle);
        return;
    }

    SessionId = NewRoomId;
    Body->TryGetStringField(TEXT("match_id"), MatchId);

    SetSessionState(EGameSessionState::InSession);
    UE_LOG(LogZombieKing, Log, TEXT("[会话] 已加入房间 %s（实时 Match %s）"),
        *SessionId, MatchId.IsEmpty() ? TEXT("无") : *MatchId);
}

void UGameSessionSubsystem::HandleLeaveSessionResponse(const FGameBackendResponse& InResponse)
{
    if (!InResponse.bSuccess)
    {
        // 离开失败不阻塞玩家：本地仍收回会话上下文并回到空闲态（否则玩家会卡在"退出中"无法再操作）。
        // 仅记录错误供 UI 提示与排障；席位回收由服务端房间生命周期兜底。
        LastError = InResponse.ErrorMessage;
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 离开请求失败（本地仍按已离开处理）：%s"), *LastError);
    }

    ResetSessionContext();
    SetSessionState(EGameSessionState::Idle);
}

void UGameSessionSubsystem::HandleStartSessionResponse(const FGameBackendResponse& InResponse)
{
    if (!InResponse.bSuccess)
    {
        // 开局失败（如非房主 NOT_OWNER / 人数不足）：仍停留在会话内，玩家可修正后重试
        LastError = InResponse.ErrorMessage;
        UE_LOG(LogZombieKing, Warning, TEXT("[会话] 开局失败：%s（仍在房间内）"), *LastError);
        SetSessionState(EGameSessionState::InSession);
        return;
    }

    const TSharedPtr<FJsonObject> Body = DeserializeJsonObject(UGameBackendSubsystem::DecodeRpcPayload(InResponse.BodyJson));
    if (Body.IsValid())
    {
        FString Err;
        if (Body->TryGetStringField(TEXT("err"), Err) && !Err.IsEmpty())
        {
            LastError = Err;
            UE_LOG(LogZombieKing, Warning, TEXT("[会话] 开局被服务端拒绝：%s（仍在房间内）"), *LastError);
            SetSessionState(EGameSessionState::InSession);
            return;
        }
    }

    // 开局成功：服务端已删房并把全员写入撮合队列（party 整队）。
    // 本地房间上下文收回——后续"进入对局"由撮合结果（queue_status / zk_match 广播）驱动，不再属于房间会话。
    UE_LOG(LogZombieKing, Log, TEXT("[会话] 开局成功，房间已解散，进入撮合队列"));
    ResetSessionContext();
    SetSessionState(EGameSessionState::Idle);
}

void UGameSessionSubsystem::SetSessionState(EGameSessionState NewState)
{
    // 幂等：状态未变化时不广播，避免表现层收到重复事件
    if (SessionState == NewState)
    {
        return;
    }

    // 先记录旧状态，再迁移：广播需要同时给出"新"与"旧"，供表现层做差异动画/提示
    const EGameSessionState OldState = SessionState;
    SessionState = NewState;

    // 广播：UI / Lua 表现层订阅（C++ 侧不据此做规则判断）
    OnSessionStateChanged.Broadcast(NewState, OldState);
}
