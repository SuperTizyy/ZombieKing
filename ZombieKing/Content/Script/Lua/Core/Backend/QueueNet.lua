-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/QueueNet.lua
-- 模块路径: Lua.Core.Backend.QueueNet
--
-- 撮合规则层：单人排队 / 房间开局后取票（rpc_queue_status 轮询，2s 节奏）。
-- 规则（可热更）：轮询间隔、超时放弃、失败提示在本文件；
-- 机制：定时器与传输全在 C++（Realtime.StartRepeating / Rpc）。
-- 说明：房间开局路径不经过本模块的 Enter——开局后服务端已写队列，
--       客户端只需 StartPolling 取票。
-- =============================================================================

local Rpc = require("Lua.Core.Backend.Rpc")

local QueueNet = {}

local realtime
local matchedCb
local polling = false
local waitedTicks = 0
local TIMEOUT_TICKS = 150 -- 2s × 150 = 5 分钟排队超时（规则值，可热更）

local function stopPolling()
    polling = false
    waitedTicks = 0
    realtime.StopRepeating("queue_status")
end

local function onRepeating(name)
    if name ~= "queue_status" or not polling then return end
    waitedTicks = waitedTicks + 1
    Rpc.Call(Rpc.Id.QueueStatus, {}, function(ok, res)
        if not polling then return end -- 回调期间已取消
        if not ok then return end     -- 轮询失败静默重试
        if res.matched then
            local cb = matchedCb; matchedCb = nil
            stopPolling()
            if cb then cb(true, { match_id = res.match_id, join_token = res.join_token }) end
        elseif not res.queued then
            local cb = matchedCb; matchedCb = nil
            stopPolling()
            if cb then cb(false, "已不在队列中") end
        elseif waitedTicks >= TIMEOUT_TICKS then
            local cb = matchedCb; matchedCb = nil
            Rpc.Call(Rpc.Id.QueueCancel, {}, function() end)
            stopPolling()
            if cb then cb(false, "排队超时") end
        end
    end)
end

function QueueNet.Init(rt)
    realtime = rt
    rt.OnRepeating:Add(onRepeating)
end

--- 开始取票轮询（Enter 与 StartPolling 共用；cb(ok, {match_id, join_token} | err)）
function QueueNet.StartPolling(cb)
    matchedCb = cb
    polling = true
    waitedTicks = 0
    realtime.StartRepeating("queue_status", 2)
end

--- 单人排队：Enter(mode, cb)
function QueueNet.Enter(mode, cb)
    Rpc.Call(Rpc.Id.QueueEnter, { mode = mode }, function(ok, res)
        if not ok then cb(false, res) return end
        QueueNet.StartPolling(cb)
    end)
end

--- 取消排队
function QueueNet.Cancel()
    if polling then
        Rpc.Call(Rpc.Id.QueueCancel, {}, function() end)
        stopPolling()
        matchedCb = nil
    end
end

function QueueNet.IsPolling() return polling end

return QueueNet
