-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/LobbyNet.lua
-- 模块路径: Lua.Core.Backend.LobbyNet
--
-- 大厅规则层：世界频道进出、聊天收发、在线列表、心跳。
-- 协议口径（Nakama v3.41 / rtapi v1.48，实测）：
--   channel_join{target=group_id, type=3(GROUP)}；content 为 JSON 字符串；
--   服务端要求调用者是群成员 —— rpc_lobby_channel_get 已自动拉入群。
-- 纪律：本文件只做"何时发、收到怎么办"的规则；传输与定时器机制全在 C++。
-- =============================================================================

local Rpc = require("Lua.Core.Backend.Rpc")
local json = require("Lua.Core.Backend.json")

local LobbyNet = {}

local realtime
local groupId       -- 大厅频道所属群（channel_get 下发）
local channelId     -- 实时通道 id（channel_join 回执下发，形如 "3.<group_id>.."）
local joinCb
local chatListeners = {}
local presenceListeners = {}

--- 初始化（编排层在 Rpc.Init 之后调用）
function LobbyNet.Init(rt)
    realtime = rt
    rt.OnEvent:Add(onEnvelope)
    rt.OnRepeating:Add(onRepeating)
end

local function onRepeating(name)
    if name == "lobby_heartbeat" then
        -- 心跳失败静默（限流/抖动不打断大厅；下一轮自动再发）
        Rpc.Call(Rpc.Id.LobbyHeartbeat, {}, function() end)
    end
end

local function onEnvelope(envJson)
    local env = json.decode(envJson)
    if not env then return end

    if env.channel then
        -- channel_join 回执：记录通道并启动心跳
        channelId = env.channel.id
        realtime.StartRepeating("lobby_heartbeat", 30)
        if joinCb then
            local cb = joinCb; joinCb = nil
            cb(true, { group_id = groupId, channel_id = channelId })
        end
    elseif env.channel_message then
        local m = env.channel_message
        -- v1.48 实测：channel_message.content 为 JSON 字符串（与发送侧同口径）
        local content = json.decode(m.content or "") or {}
        for _, fn in ipairs(chatListeners) do
            fn(content.text or "", m.username or "", m.user_id or "")
        end
    elseif env.channel_presence_event then
        local ev = env.channel_presence_event
        for _, fn in ipairs(presenceListeners) do
            fn(ev.joins or {}, ev.leaves or {})
        end
    elseif env.error then
        if joinCb then
            local cb = joinCb; joinCb = nil
            cb(false, "频道加入被拒: " .. tostring(env.error.message))
        end
    end
end

--- 进入大厅频道：cb(ok, infoOrErr)
function LobbyNet.EnterLobby(cb)
    Rpc.Call(Rpc.Id.LobbyChannelGet, {}, function(ok, res)
        if not ok then cb(false, res) return end
        groupId = res.group_id
        joinCb = cb
        local cid = realtime.SendEnvelope(json.encode({
            channel_join = { target = groupId, type = 3 },  -- 3 = GROUP（v1.48 枚举）
        }))
        if cid == "" then
            joinCb = nil
            cb(false, "实时通道未连接")
        end
    end)
end

--- 离开大厅频道（停心跳 + 退通道）
function LobbyNet.LeaveLobby()
    realtime.StopRepeating("lobby_heartbeat")
    if channelId then
        realtime.SendEnvelope(json.encode({ channel_leave = { channel_id = channelId } }))
        channelId = nil
    end
end

--- 发送大厅聊天（content 为 JSON 字符串——v1.48 协议口径）
function LobbyNet.SendChat(text)
    if not channelId or text == "" then return false end
    local cid = realtime.SendEnvelope(json.encode({
        channel_message_send = { channel_id = channelId, content = json.encode({ text = text }) },
    }))
    return cid ~= ""
end

--- 注册聊天监听：fn(text, username, userId)
function LobbyNet.AddChatListener(fn) table.insert(chatListeners, fn) end

--- 注册频道成员进出监听：fn(joins, leaves)
function LobbyNet.AddPresenceListener(fn) table.insert(presenceListeners, fn) end

--- 拉取在线列表：cb(ok, listOrErr)，list = { {user_id, username}, ... }
function LobbyNet.RefreshOnline(cb)
    Rpc.Call(Rpc.Id.OnlineList, {}, function(ok, res)
        if not ok then cb(false, res) return end
        cb(true, res.online or {})
    end)
end

function LobbyNet.GetChannelId() return channelId end

return LobbyNet
