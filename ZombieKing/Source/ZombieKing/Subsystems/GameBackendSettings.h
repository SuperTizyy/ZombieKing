#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"   // UDeveloperSettings（配置暴露到 Project Settings 并持久化到 ini）
#include "GameBackendSettings.generated.h"

/**
 * 竞技生化网游：自研后端的连接配置
 * 位置：Project Settings → Game → ZombieKing 后端（也可直接改 Config/DefaultGame.ini 的 [/Script/ZombieKing.GameBackendSettings]）
 *
 * 为什么用 UDeveloperSettings 而不是写死在 C++：
 *  - 后端地址、超时、重试、并发上限属于**环境配置**而非代码逻辑：测试环境/预发布/正式环境各不相同；
 *  - 存 ini 即可热更（改配置不用重编译 C++），并能在编辑器里可视化编辑、避免手改 ini 出错；
 *  - 这是 UE 项目里"项目级配置"的行业标准做法（对齐 Lyra 的 UDeveloperSettings 用法）。
 *
 * 注意：本类只放"连接机制"参数，不放任何**业务规则**（几人房、何时开赛等一律在 Lua 规则脚本里）。
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "ZombieKing 后端"))
class ZOMBIEKING_API UGameBackendSettings : public UDeveloperSettings
{
    GENERATED_BODY()

public:
    // -------------------------------------------------------------------------
    // 连接
    // -------------------------------------------------------------------------
    /** 后端基址（不含结尾斜杠）：Nakama REST 网关（本地开发 7350） */
    UPROPERTY(Config, EditAnywhere, Category = "连接")
    FString BaseUrl = TEXT("http://127.0.0.1:7350");

    /** Nakama socket.server_key：设备登录端点的 Basic 鉴权凭据（仅内网/本地开发使用，正式环境走部署注入） */
    UPROPERTY(Config, EditAnywhere, Category = "连接")
    FString ServerKey;

    /** 实时通道（WebSocket）基址：Nakama /ws 端点，令牌由 GameRealtimeSubsystem 追加 */
    UPROPERTY(Config, EditAnywhere, Category = "连接")
    FString WsUrl = TEXT("ws://127.0.0.1:7350/ws");

    /** 单次请求超时（秒） */
    UPROPERTY(Config, EditAnywhere, Category = "连接", meta = (ClampMin = "1.0"))
    float RequestTimeoutSeconds = 10.0f;

    /**
     * 默认最大重试次数（仅对"显式标记为可重试"的请求生效）。
     * 说明：非幂等接口（创建会话/加入会话）绝不应盲目重试——请求可能已送达而回执丢失，
     *       重试会造成重复建房/重复入房，故重试与否由调用方逐请求显式声明。
     */
    UPROPERTY(Config, EditAnywhere, Category = "连接", meta = (ClampMin = "0"))
    int32 DefaultMaxRetries = 2;

    /** 同时进行的最大请求数（超出者排队等待）；防止弱网/断网时请求风暴拖垮客户端与后端 */
    UPROPERTY(Config, EditAnywhere, Category = "连接", meta = (ClampMin = "1"))
    int32 MaxConcurrentRequests = 8;

    /** 是否启用后端：置 false 时所有请求立即以失败回执返回（便于纯单机/离线调试） */
    UPROPERTY(Config, EditAnywhere, Category = "连接")
    bool bBackendEnabled = true;

    // -----------------------------------------------------------------
    // 鉴权（本轮只做"注入点"，账号体系另行立项）
    // -----------------------------------------------------------------
    /** 鉴权头字段名（可配以便与后端约定对齐，默认 Authorization） */
    UPROPERTY(Config, EditAnywhere, Category = "鉴权")
    FString AuthHeaderName = TEXT("Authorization");

    /** 鉴权头前缀（如 "Bearer "；置空则直接把令牌写入头值） */
    UPROPERTY(Config, EditAnywhere, Category = "鉴权")
    FString AuthHeaderPrefix = TEXT("Bearer ");

    /** Project Settings 中的归类：放到 Game 分类下，与玩法类项目配置并列 */
    virtual FName GetCategoryName() const override;
};
