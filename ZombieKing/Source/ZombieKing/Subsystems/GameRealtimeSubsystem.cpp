#include "GameRealtimeSubsystem.h"
#include "GameBackendSettings.h"      // UGameBackendSettings（WsUrl 配置）
#include "WebSocketsModule.h"         // FWebSocketsModule（引擎 WS 模块入口）
#include "IWebSocket.h"               // IWebSocket（底层连接对象）
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "ZombieKing.h"               // LogZombieKing（实时通道日志）

namespace
{
    /** 非主动断开后的重连间隔（秒，机制层固定节奏；规则层如需退避可在 Lua 侧另行控制） */
    constexpr float ReconnectDelaySeconds = 5.0f;
}

FName UGameRealtimeSubsystem::GetServiceName() const
{
    return FName(TEXT("Realtime"));
}

void UGameRealtimeSubsystem::OnServiceInitialize()
{
    bOpen = false;
    bExplicitDisconnect = false;
    Token.Reset();
    NextCid = 1;
}

void UGameRealtimeSubsystem::OnServiceDeinitialize()
{
    // 主动关闭：不再重连；清掉所有 ticker 与 socket
    bExplicitDisconnect = true;
    if (ReconnectTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTicker);
        ReconnectTicker.Reset();
    }
    for (auto& Pair : RepeatTickers)
    {
        FTSTicker::GetCoreTicker().RemoveTicker(Pair.Value);
    }
    RepeatTickers.Reset();

    if (Socket.IsValid())
    {
        Socket->Close();
        Socket.Reset();
    }
    bOpen = false;
}

bool UGameRealtimeSubsystem::Connect(const FString& SessionToken)
{
    if (SessionToken.IsEmpty())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[实时] 连接失败：会话令牌为空"));
        return false;
    }

    // 重入保护：已有连接先主动断开，再以新令牌重建
    if (Socket.IsValid())
    {
        Socket->Close();
        Socket.Reset();
    }
    if (ReconnectTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTicker);
        ReconnectTicker.Reset();
    }

    Token = SessionToken;
    bExplicitDisconnect = false;
    OpenSocket();
    return true;
}

void UGameRealtimeSubsystem::OpenSocket()
{
    const UGameBackendSettings* Settings = GetDefault<UGameBackendSettings>();
    if (Settings->WsUrl.IsEmpty() || Token.IsEmpty())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[实时] 连接失败：WsUrl 或令牌未配置"));
        return;
    }

    // Nakama WS 口径：token 走查询参数，JSON 协议为默认格式（无需 subprotocol）
    const FString Url = Settings->WsUrl + TEXT("?token=") + Token;
    Socket = FWebSocketsModule::Get().CreateWebSocket(Url);

    const TWeakObjectPtr<UGameRealtimeSubsystem> WeakThis(this);

    Socket->OnConnected().AddLambda([WeakThis]()
    {
        if (UGameRealtimeSubsystem* Self = WeakThis.Get())
        {
            Self->bOpen = true;
            Self->OnConnectionChanged.Broadcast(true);
            UE_LOG(LogZombieKing, Log, TEXT("[实时] 已连接"));
        }
    });

    Socket->OnConnectionError().AddLambda([WeakThis](const FString& Error)
    {
        if (UGameRealtimeSubsystem* Self = WeakThis.Get())
        {
            UE_LOG(LogZombieKing, Warning, TEXT("[实时] 连接错误：%s"), *Error);
            Self->HandleConnectionClosed();
        }
    });

    Socket->OnMessage().AddLambda([WeakThis](const FString& Message)
    {
        if (UGameRealtimeSubsystem* Self = WeakThis.Get())
        {
            Self->HandleIncomingMessage(Message);
        }
    });

    // UE5.7 实测 OnClosed 为三参（含 bWasClean）
    Socket->OnClosed().AddLambda([WeakThis](int32 Code, const FString& Reason, bool /*bWasClean*/)
    {
        if (UGameRealtimeSubsystem* Self = WeakThis.Get())
        {
            UE_LOG(LogZombieKing, Log, TEXT("[实时] 连接关闭（code=%d reason=%s）"), Code, *Reason);
            Self->HandleConnectionClosed();
        }
    });

    Socket->Connect();
}

void UGameRealtimeSubsystem::Disconnect()
{
    bExplicitDisconnect = true;
    Token.Reset();
    if (ReconnectTicker.IsValid())
    {
        FTSTicker::GetCoreTicker().RemoveTicker(ReconnectTicker);
        ReconnectTicker.Reset();
    }
    if (Socket.IsValid())
    {
        Socket->Close();
        Socket.Reset();
    }
    if (bOpen)
    {
        bOpen = false;
        OnConnectionChanged.Broadcast(false);
    }
}

FString UGameRealtimeSubsystem::SendEnvelope(const FString& CmdObjectJson)
{
    if (!bOpen || !Socket.IsValid() || !Socket->IsConnected())
    {
        return FString();
    }

    // 解析命令对象并注入 cid（机制层保证同连接内回执可区分）
    TSharedPtr<FJsonObject> Command;
    const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(CmdObjectJson);
    if (!FJsonSerializer::Deserialize(Reader, Command) || !Command.IsValid())
    {
        UE_LOG(LogZombieKing, Warning, TEXT("[实时] 发送失败：命令 JSON 非法"));
        return FString();
    }

    const FString Cid = FString::FromInt(NextCid++);
    Command->SetStringField(TEXT("cid"), Cid);

    FString Out;
    const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
    FJsonSerializer::Serialize(Command.ToSharedRef(), Writer);
    Socket->Send(Out);
    return Cid;
}

void UGameRealtimeSubsystem::HandleIncomingMessage(const FString& Message)
{
    // 纯机制：不理解内容，原文广播（Lua 按 Envelope 字段分派）
    OnEvent.Broadcast(Message);
}

void UGameRealtimeSubsystem::HandleConnectionClosed()
{
    if (bOpen)
    {
        bOpen = false;
        OnConnectionChanged.Broadcast(false);
    }

    if (bExplicitDisconnect || Token.IsEmpty() || !IsServiceReady())
    {
        return; // 主动断开或服务关闭：不重连
    }

    // 非主动断开：安排一次性重连 ticker（弱引用守护，服务销毁后安全跳过）
    if (!ReconnectTicker.IsValid())
    {
        const TWeakObjectPtr<UGameRealtimeSubsystem> WeakThis(this);
        ReconnectTicker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
            [WeakThis](float)
            {
                if (UGameRealtimeSubsystem* Self = WeakThis.Get())
                {
                    Self->TryReconnect();
                }
                return false; // 一次性
            }), ReconnectDelaySeconds);
    }
}

void UGameRealtimeSubsystem::TryReconnect()
{
    ReconnectTicker.Reset();
    if (bExplicitDisconnect || Token.IsEmpty() || !IsServiceReady())
    {
        return;
    }
    UE_LOG(LogZombieKing, Log, TEXT("[实时] 尝试重连…"));
    if (Socket.IsValid())
    {
        Socket->Close();
        Socket.Reset();
    }
    OpenSocket();
}

void UGameRealtimeSubsystem::StartRepeating(const FString& Name, float IntervalSeconds)
{
    if (Name.IsEmpty() || IntervalSeconds < 0.1f)
    {
        return;
    }
    StopRepeating(Name); // 重复注册视为覆盖，保证一个名字只有一个 ticker

    const TWeakObjectPtr<UGameRealtimeSubsystem> WeakThis(this);
    const FString CapturedName = Name;
    const FTSTicker::FDelegateHandle Handle = FTSTicker::GetCoreTicker().AddTicker(
        FTickerDelegate::CreateLambda([WeakThis, CapturedName](float)
        {
            if (UGameRealtimeSubsystem* Self = WeakThis.Get())
            {
                Self->OnRepeating.Broadcast(CapturedName);
                return true; // 持续
            }
            return false;
        }), IntervalSeconds);
    RepeatTickers.Add(Name, Handle);
}

void UGameRealtimeSubsystem::StopRepeating(const FString& Name)
{
    if (FTSTicker::FDelegateHandle* Handle = RepeatTickers.Find(Name))
    {
        FTSTicker::GetCoreTicker().RemoveTicker(*Handle);
        RepeatTickers.Remove(Name);
    }
}
