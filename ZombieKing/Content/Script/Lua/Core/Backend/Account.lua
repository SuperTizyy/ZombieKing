-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/Account.lua
-- 模块路径: Lua.Core.Backend.Account
--
-- 账号规则层：设备号持久化 → Nakama 设备登录 → 令牌注入 → 实时通道连接。
-- 规则（可热更）：设备号存放位置、登录时机、失败重试策略在本文件；
-- 机制：HTTP/WS 传输全在 C++（Rpc / GameBackendSubsystem / GameRealtimeSubsystem）。
-- =============================================================================

local Rpc = require("Lua.Core.Backend.Rpc")

local Account = {}

local deviceId   -- 本机设备号（持久化于 Saved，重装即新号；正式账号走邮箱绑定升级）
local sessionToken
local userId

local DEVICE_ID_FILE = "zk_device_id.txt"

--- 读取/生成设备号（≥10 字节；服务端实测约束）
local function getDeviceId()
    if deviceId then return deviceId end
    local dir = UE.UKismetSystemLibrary.GetProjectSavedDirectory()
    local path = dir .. "/" .. DEVICE_ID_FILE
    local f = io.open(path, "r")
    if f then
        deviceId = (f:read("*a"):gsub("%s", ""))
        f:close()
        if #deviceId >= 10 then return deviceId end
    end
    deviceId = tostring(UE.UKismetSystemLibrary.CreateGuid()):gsub("-", "")
    local w = io.open(path, "w")
    if w then w:write(deviceId) w:close() end
    return deviceId
end

function Account.GetDeviceId() return getDeviceId() end
function Account.GetToken() return sessionToken end

--- 登录（仅取令牌；成功后由编排层调用 ActivateSession 注入令牌并接通实时通道）：cb(ok, errText)
function Account.Login(cb)
    Rpc.SendRaw(
        Rpc.AuthDevicePath,
        { id = getDeviceId() },
        "Auth.Device",
        UE.EGameBackendAuth.BasicKey,
        function(ok, result)
            if not ok then cb(false, result) return end
            sessionToken = result.token
            if not sessionToken then cb(false, "登录响应缺少 token") return end
            cb(true, nil)
        end)
end

--- 登录成功后接线：注入令牌 + 建立实时连接（由编排层在 Login 成功回调里调用）
function Account.ActivateSession(backend, realtime)
    backend.SetAuthToken(sessionToken or "")
    if sessionToken then
        realtime.Connect(sessionToken)
    end
end

return Account
