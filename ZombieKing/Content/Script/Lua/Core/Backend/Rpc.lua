-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/Rpc.lua
-- 模块路径: Lua.Core.Backend.Rpc
--
-- 后端 RPC 分发器（Lua 规则层的"发请求、收回执"统一入口）
-- 机制：所有请求经 C++ UGameBackendSubsystem（唯一 HTTP 出口）发出；
--       回执经 OnBackendResponse 广播，本模块按 RequestTag 分派给等待队列。
-- 纪律：同一 tag 的请求串行等待（同 RPC 并发调用时按发起顺序 FIFO 配对回执——
--       传输层 FIFO 出队保证同 tag 回执顺序与请求顺序一致）。
-- =============================================================================

local json = require("Lua.Core.Backend.json")

local Rpc = {}

-- 非 RPC 端点路径（与 C++ GameBackendEndpoints.h 契约同源，改动须两端对齐）
Rpc.AuthDevicePath = "/v2/account/authenticate/device?create=true"
Rpc.AuthEmailPath = "/v2/account/authenticate/email?create=false"

-- RPC id 契约（与 C++ GameBackendRpcIds / 服务端 main.ts 注册名同源）
Rpc.Id = {
    BootstrapGet     = "rpc_bootstrap_get",
    CatalogGet       = "rpc_catalog_get",
    ShopBuy          = "rpc_shop_buy",
    GachaDraw        = "rpc_gacha_draw",
    ItemEnhance      = "rpc_item_enhance",
    LoadoutSet       = "rpc_loadout_set",
    AccountBindEmail = "rpc_account_bind_email",
    LobbyChannelGet  = "rpc_lobby_channel_get",
    LobbyHeartbeat   = "rpc_lobby_heartbeat",
    OnlineList       = "rpc_online_list",
    RoomCreate       = "rpc_room_create",
    RoomJoin         = "rpc_room_join",
    RoomLeave        = "rpc_room_leave",
    RoomList         = "rpc_room_list",
    RoomStart        = "rpc_room_start",
    QueueEnter       = "rpc_queue_enter",
    QueueCancel      = "rpc_queue_cancel",
    QueueStatus      = "rpc_queue_status",
}

local Backend          -- UGameBackendSubsystem
local pending = {}     -- tag(string) -> 回调队列
local handle           -- OnBackendResponse 委托句柄

--- 回执分派（游戏线程）
local function onResponse(response)
    local tag = tostring(response.RequestTag)
    local queue = pending[tag]
    if not queue or #queue == 0 then return end
    local cb = table.remove(queue, 1)
    if not response.bSuccess then
        cb(false, response.ErrorMessage)
        return
    end
    -- RPC 回执统一解 payload；登录等非 RPC 回执原样解析
    local bodyText = UE.UGameBackendSubsystem.DecodeRpcPayload(response.BodyJson)
    local result, err = json.decode(bodyText)
    if not result then
        cb(false, "响应解析失败: " .. tostring(err))
        return
    end
    -- 服务端业务错误经 RPC throw 走非 2xx（上面 bSuccess 分支已拦截）；
    -- 兼容 200 + {err:"..."} 形态
    if result.err then
        cb(false, result.err)
        return
    end
    cb(true, result)
end

--- 初始化（GameInstance 服务就绪后由编排层调用一次）
function Rpc.Init(backend)
    Backend = backend
    if handle then
        Backend.OnBackendResponse:Remove(handle)
        handle = nil
    end
    handle = Backend.OnBackendResponse:Add(onResponse)
end

--- 发送 RPC：cb(ok, resultTableOrErrText)
--- rpcId 取值见 C++ GameBackendRpcIds（字符串常量与契约一致）
function Rpc.Call(rpcId, bodyTable, cb)
    if not Backend then cb(false, "后端服务未初始化") return end
    local tag = "Rpc." .. rpcId
    pending[tag] = pending[tag] or {}

    local req = UE.FGameBackendRequest()
    req.Path = UE.UGameBackendSubsystem.BuildRpcPath(rpcId)
    req.BodyJson = UE.UGameBackendSubsystem.EncodeRpcPayload(json.encode(bodyTable or {}))
    req.RequestTag = tag
    req.Auth = UE.EGameBackendAuth.Bearer
    table.insert(pending[tag], cb)
    Backend:SendRequest(req)
end

--- 发送原始请求（登录等非 RPC 端点；body 为普通 JSON，不双重编码）
function Rpc.SendRaw(path, bodyTable, tag, authMode, cb)
    if not Backend then cb(false, "后端服务未初始化") return end
    pending[tag] = pending[tag] or {}
    local req = UE.FGameBackendRequest()
    req.Path = path
    req.BodyJson = json.encode(bodyTable or {})
    req.RequestTag = tag
    req.Auth = authMode
    table.insert(pending[tag], cb)
    Backend:SendRequest(req)
end

return Rpc
