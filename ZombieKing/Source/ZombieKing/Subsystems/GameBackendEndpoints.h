#pragma once

#include "CoreMinimal.h"

/**
 * 竞技生化网游：后端（Nakama v3.41 + TS 权威模块）的**客户端契约**（端点路径 + 请求标签 + RPC id）
 *
 * 为什么这些常量放 C++ 而不是 Lua：
 *  - 它们描述的是"协议"（端点路径与字段名），属于**不可热更的契约骨架**（同"网络复制声明"一类）；
 *    改协议本就意味着前后端要一起改，天然需要重新构建；
 *  - 而"要不要建房、几人房、超时多久"等**规则与数值**一律在 Lua（可热更），二者职责不重叠。
 *
 * 协议口径（与 ZombieKingServer 实测一致，改动须同版本对齐）：
 *  - 登录：POST /v2/account/authenticate/device?create=true，Basic 鉴权（用户名=socket.server_key，密码空），
 *          体为**普通 JSON** {"id":"<设备号,≥10字节>"}；回执 {"token","refresh_token","created"}。
 *  - RPC：POST /v2/rpc/<rpc_id>，Bearer 会话令牌；**请求体是 JSON 字符串的双重编码**
 *          （即 body = JSON.stringify(业务JSON字符串)，见 UGameBackendSubsystem::EncodeRpcPayload）；
 *          响应体为 {"payload":"<业务JSON字符串>"}（见 DecodeRpcPayload 解包）。
 *  - 实时：WebSocket（见 UGameRealtimeSubsystem），rtapi v1.48 JSON 协议：
 *          channel_join{target,type:3=GROUP} / channel_message_send{channel_id,content:JSON字符串} /
 *          match_join{match_id} / match_data_send{match_id,op_code,data:base64}。
 *
 * 错误约定：非 2xx 一律视为业务失败；Nakama 错误体 {"code":N,"message":"..."}，
 *           调用方（Lua 规则）自行解释 message 前缀错误码（如 RATE_LIMITED/ROOM_FULL）。
 */
namespace GameBackendEndpoints
{
    /** 设备登录（Nakama 原生鉴权；Basic 鉴权，无 Bearer） */
    const TCHAR* const AuthDevice = TEXT("/v2/account/authenticate/device?create=true");
    /** 邮箱密码登录（账号绑定后使用；Basic 鉴权） */
    const TCHAR* const AuthEmail = TEXT("/v2/account/authenticate/email?create=false");
    /** RPC 路径前缀（拼接 rpc_id 使用：/v2/rpc/<rpc_id>） */
    const TCHAR* const RpcPrefix = TEXT("/v2/rpc/");
}

/**
 * RPC id 契约（服务端 nakama/modules/src/main.ts 注册名为唯一口径；改名必须两端同步）。
 */
namespace GameBackendRpcIds
{
    // 资产域
    const TCHAR* const BootstrapGet = TEXT("rpc_bootstrap_get");
    const TCHAR* const CatalogGet = TEXT("rpc_catalog_get");
    const TCHAR* const ShopBuy = TEXT("rpc_shop_buy");
    const TCHAR* const GachaDraw = TEXT("rpc_gacha_draw");
    const TCHAR* const ItemEnhance = TEXT("rpc_item_enhance");
    const TCHAR* const LoadoutSet = TEXT("rpc_loadout_set");
    // 账号域
    const TCHAR* const AccountBindEmail = TEXT("rpc_account_bind_email");
    // 大厅域
    const TCHAR* const LobbyChannelGet = TEXT("rpc_lobby_channel_get");
    const TCHAR* const LobbyHeartbeat = TEXT("rpc_lobby_heartbeat");
    const TCHAR* const OnlineList = TEXT("rpc_online_list");
    // 房间域（会话）
    const TCHAR* const RoomCreate = TEXT("rpc_room_create");
    const TCHAR* const RoomJoin = TEXT("rpc_room_join");
    const TCHAR* const RoomLeave = TEXT("rpc_room_leave");
    const TCHAR* const RoomList = TEXT("rpc_room_list");
    const TCHAR* const RoomStart = TEXT("rpc_room_start");
    // 撮合域
    const TCHAR* const QueueEnter = TEXT("rpc_queue_enter");
    const TCHAR* const QueueCancel = TEXT("rpc_queue_cancel");
    const TCHAR* const QueueStatus = TEXT("rpc_queue_status");
}

/**
 * 请求标签：用于把"回执"与"发起方"对应起来（见 UGameBackendSubsystem::OnBackendResponse）。
 * 命名约定：<域>.<动作>，便于 Lua 侧按前缀分派与日志过滤。
 * 说明：这里登记为字符串常量而非 FName 全局量——避免在静态初始化阶段构造 FName（FName 依赖 Core 的全局表，
 *       跨模块静态初始化顺序不确定），在使用处现场构造 FName 更稳妥。
 */
namespace GameBackendRequestTags
{
    const TCHAR* const AuthDevice = TEXT("Auth.Device");
    const TCHAR* const AuthEmail = TEXT("Auth.Email");
    /** Lua 直发 RPC 的通用标签（Lua 按 Rpc 前缀 + 自身关联表分派具体 RPC id） */
    const TCHAR* const Rpc = TEXT("Backend.Rpc");
    const TCHAR* const SessionCreate = TEXT("Session.Create");
    const TCHAR* const SessionJoin = TEXT("Session.Join");
    const TCHAR* const SessionLeave = TEXT("Session.Leave");
    const TCHAR* const SessionStart = TEXT("Session.Start");
    const TCHAR* const SessionList = TEXT("Session.List");
    const TCHAR* const SessionHeartbeat = TEXT("Session.Heartbeat");
}
