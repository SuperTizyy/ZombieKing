-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/RoomNet.lua
-- 模块路径: Lua.Core.Backend.RoomNet
--
-- 房间规则层：建房/加入/离开/开局（REST 走 C++ 会话服务状态机）+
--             房间聊天（WS 实时 Match，op_code=1 广播）。
-- 协议口径（Nakama v3.41 / rtapi v1.48，实测）：
--   match_data_send{match_id, op_code, data(base64)}；matchLoop 广播回 match_data；
--   服务端 zk_lobby 按 game.room_member 鉴权，非成员 join 被拒。
-- =============================================================================

local Rpc = require("Lua.Core.Backend.Rpc")
local json = require("Lua.Core.Backend.json")

local RoomNet = {}

local session, realtime
local matchId
local roomChatListeners = {}
local presenceListeners = {}

-- ---------------------------------------------------------------------------
-- WS Envelope 分派
-- ---------------------------------------------------------------------------

local function onEnvelope(envJson)
    local env = json.decode(envJson)
    if not env then return end

    if env.match then
        -- match_join 回执（含已在房成员 presences）
        if env.match.match_id then matchId = env.match.match_id end
    elseif env.match_data then
        local md = env.match_data
        local op = tonumber(md.op_code) or tonumber(md.opCode) or 0
        if op == 1 and md.data then
            local okDecode, plain = UE.UKismetEncodingLibrary.Base64Decode(md.data)
            if okDecode then
                local msg = json.decode(plain)
                if msg then
                    for _, fn in ipairs(roomChatListeners) do
                        fn(msg.text or "", md.username or "", md.user_id or "")
                    end
                end
            end
        end
    elseif env.match_presence_event then
        local ev = env.match_presence_event
        for _, fn in ipairs(presenceListeners) do
            fn(ev.joins or {}, ev.leaves or {})
        end
    end
end

--- 会话进入"会话内"后自动加入房间实时 Match
local function joinRoomMatch()
    local mid = session.GetMatchId()
    if mid and mid ~= "" then
        matchId = mid
        realtime.SendEnvelope(json.encode({ match_join = { match_id = mid } }))
    end
end

local function onSessionStateChanged(newState, oldState)
    local InSession = UE.EGameSessionState.InSession
    local Idle = UE.EGameSessionState.Idle
    if newState == InSession then
        joinRoomMatch()
    elseif newState == Idle then
        matchId = nil
    end
end

-- ---------------------------------------------------------------------------
-- 一次性"入房完成"等待（状态机事件驱动）
-- ---------------------------------------------------------------------------

local function onceEntered(cb)
    local handle
    handle = session.OnSessionStateChanged:Add(function(newState, oldState)
        local InSession = UE.EGameSessionState.InSession
        local Idle = UE.EGameSessionState.Idle
        if newState == InSession then
            session.OnSessionStateChanged:Remove(handle)
            cb(true, { room_id = session.GetSessionId(), match_id = session.GetMatchId() })
        elseif newState == Idle and oldState == UE.EGameSessionState.Connecting then
            -- 建立中 → 空闲：创建/加入失败
            session.OnSessionStateChanged:Remove(handle)
            cb(false, session.GetLastError())
        end
    end)
end

-- ---------------------------------------------------------------------------
-- 对外接口（供 UI / 编排层调用）
-- ---------------------------------------------------------------------------

function RoomNet.Init(sess, rt)
    session, realtime = sess, rt
    rt.OnEvent:Add(onEnvelope)
    sess.OnSessionStateChanged:Add(onSessionStateChanged)
end

--- 建房：cb(ok, infoOrErr)（成功后自动进房间 Match）
function RoomNet.Create(mode, maxPlayers, cb)
    onceEntered(cb)
    if not session.RequestCreateSession(mode, maxPlayers) then
        -- 受理失败（参数/状态/服务不可用）：立即以错误收尾
        cb(false, session.GetLastError())
    end
end

--- 加入房间（知道 room_id 即视为受邀）
function RoomNet.Join(roomId, cb)
    onceEntered(cb)
    if not session.RequestJoinSession(roomId) then
        cb(false, session.GetLastError())
    end
end

--- 离开房间（房主离开=解散）
function RoomNet.Leave(cb)
    matchId = nil
    session.RequestLeaveSession()
    if cb then cb(true) end
end

--- 房主开局：成功后房间解散、全员进撮合队列（后续由 QueueNet 接管）。
--- 失败（NOT_OWNER/人数不足）服务端不改状态，故用 1s 轮询兜底通知。
function RoomNet.Start(cb)
    local handle, ticks
    ticks = 0
    local done = false
    local function finish(ok, err)
        if done then return end
        done = true
        session.OnSessionStateChanged:Remove(handle)
        realtime.StopRepeating("room_start_check")
        cb(ok, err)
    end
    handle = session.OnSessionStateChanged:Add(function(newState, oldState)
        if newState == UE.EGameSessionState.Idle and oldState == UE.EGameSessionState.InSession then
            matchId = nil
            finish(true)
        end
    end)
    -- 兜底轮询：5 秒内状态未迁移视为失败（服务端把错误留在 LastError）
    realtime.StartRepeating("room_start_check", 1)
    local checkHandle
    checkHandle = realtime.OnRepeating:Add(function(name)
        if name ~= "room_start_check" then return end
        ticks = ticks + 1
        if session.IsInSession() then
            if ticks >= 5 then
                realtime.OnRepeating:Remove(checkHandle)
                finish(false, session.GetLastError())
            end
        else
            realtime.OnRepeating:Remove(checkHandle)
        end
    end)
    session.RequestStartSession()
end

--- 发送房间聊天（op_code=1；data 为 base64(JSON 字符串)）
function RoomNet.SendRoomChat(text)
    if not matchId or text == "" then return false end
    local okEnc, b64 = UE.UKismetEncodingLibrary.Base64Encode(json.encode({ text = text }))
    if not okEnc then return false end
    local cid = realtime.SendEnvelope(json.encode({
        match_data_send = { match_id = matchId, op_code = 1, data = b64 },
    }))
    return cid ~= ""
end

function RoomNet.AddRoomChatListener(fn) table.insert(roomChatListeners, fn) end
function RoomNet.AddPresenceListener(fn) table.insert(presenceListeners, fn) end
function RoomNet.GetRoomId() return session.GetSessionId() end
function RoomNet.GetMatchId() return matchId end

--- 房间列表（只读；cb(ok, roomsOrErr)）
function RoomNet.List(cb)
    Rpc.Call(Rpc.Id.RoomList, {}, function(ok, res)
        if not ok then cb(false, res) return end
        cb(true, res.rooms or {})
    end)
end

return RoomNet
