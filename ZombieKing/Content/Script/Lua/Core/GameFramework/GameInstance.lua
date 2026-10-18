-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/GameFramework/GameInstance.lua
-- 模块路径: Lua.Core.GameFramework.GameInstance
--            （由 C++ 的 UGameGameInstance 通过 IUnLuaInterface 绑定；层级用 . 连接）
--
-- 竞技生化网游：全局游戏实例脚本（跨关卡存活，进程内唯一）
-- 职责划分（工业级 UnLua 分层约定）：
--   * C++ 层（UGameGameInstance）：只做生命周期与事件派发 —— Super::Init 装配服务层、
--     派发 OnGameInstanceInit / OnGameInstanceShutdown / OnLanguageChanged、经 IUnLuaInterface 绑定本脚本；
--   * Lua 层（本文件）：GameInstance 级"规则/流程"逻辑 —— 全局配置加载、服务层编排、
--     存档冲刷、遥测上报时机、本地化文案层接入等。写在这里的核心收益：线上可不停机热更、无需重编译 C++。
--   * 注意：UnLua 的启动入口是 Lua.Core.Bootstrap（见 Content/Script/Lua/Core/Bootstrap.lua），
--     那是"环境引导"脚本，与本文件无关。
--
-- 本脚本可用的 C++ 契约（均已提供，改动数据结构才需动 C++）：
--   self                            -- 即 UGameGameInstance 自身
--   self:OnGameInstanceInit()       -- 生命周期事件：C++ 在"服务层就绪之后"调用（见下方同名实现）
--   self:OnGameInstanceShutdown()   -- 生命周期事件：C++ 在关闭前调用（见下方同名实现）
--   self:OnLanguageChanged(Code)    -- 语言变更事件：C++ 把语言落地引擎 culture 后派发（含启动期首次推送）
--   取 GameInstance 层服务（会话 / 遥测 / 设置 / 本地化…）：
--     引擎未对 Lua 暴露模板化的 GameInstance:GetSubsystem，须走引擎自带的蓝图入口：
--     UE.USubsystemBlueprintLibrary.GetGameInstanceSubsystem(<有世界的上下文>, UE.<服务类>)
--     —— 注意"有世界的上下文"：GameInstance 在启动早期可能尚无世界，此时该函数会返回 nil；
--        故凡是"启动期就要取服务"的需求，都应由 C++ 主动派发事件/数据下来（如 OnLanguageChanged），
--        不要在 Lua 的 OnInit 里硬取服务。
--   取 World 层服务（反作弊）：
--     UE.USubsystemBlueprintLibrary.GetWorldSubsystem(<世界上下文>, UE.UGameAntiCheatSubsystem)
-- =============================================================================

-- Lua 文案层（玩法文案的唯一出口；UI 文案走 C++ 的 UE 本地化表，见 UGameLocalizationSubsystem:GetUIText）
local Localization = require("Lua.Core.Localization.Localization")

-- 后端接入层（规则模块；机制全在 C++ 服务）
local Rpc = require("Lua.Core.Backend.Rpc")
local Account = require("Lua.Core.Backend.Account")
local LobbyNet = require("Lua.Core.Backend.LobbyNet")
local RoomNet = require("Lua.Core.Backend.RoomNet")
local QueueNet = require("Lua.Core.Backend.QueueNet")

local GameInstance = Class()

-- 后端服务句柄（OnGameInstanceInit 装配；供 UI/调试经 GameInstance.GetNet() 取用）
local net = { Lobby = LobbyNet, Room = RoomNet, Queue = QueueNet, Account = Account, Rpc = Rpc }

GameInstance.OnConnectionChanged = nil -- 编排层注入：fn(bConnected) 供 UI 显示网络状态

-- 全局初始化完成（服务层已就绪）
-- 说明：本回调由 C++ 在 Super::Init 之后派发，因此此处可安全取用各 GameInstance 层服务；
--       若在这里取 World 层服务（反作弊），需注意此时世界可能尚未创建，应延迟到进关后再取。
function GameInstance:OnGameInstanceInit()
    -- 初始化 Lua 文案层：注册听众表、把语言复位到基准语言。
    -- 真正的"当前语言"由 C++ 随后推送（见 OnLanguageChanged）—— 这样 Lua 无需在启动期去取服务，
    -- 避开了"GameInstance 此时可能还没有世界、取不到子系统"的坑。
    Localization.Init()

    -- 装配后端接入层：取三个 GameInstance 层服务 → 初始化分发器与规则模块 → 设备登录。
    local GI = self
    local Backend = UE.USubsystemBlueprintLibrary.GetGameInstanceSubsystem(GI, UE.UGameBackendSubsystem)
    local Realtime = UE.USubsystemBlueprintLibrary.GetGameInstanceSubsystem(GI, UE.UGameRealtimeSubsystem)
    local Session = UE.USubsystemBlueprintLibrary.GetGameInstanceSubsystem(GI, UE.UGameSessionSubsystem)
    if not Backend or not Realtime or not Session then
        print("[GameInstance] 后端服务未就绪，跳过网络装配")
        return
    end

    Rpc.Init(Backend)
    LobbyNet.Init(Realtime)
    RoomNet.Init(Session, Realtime)
    QueueNet.Init(Realtime)

    Realtime.OnConnectionChanged:Add(function(bConnected)
        print("[GameInstance] 实时通道 " .. (bConnected and "已连接" or "已断开"))
        if GameInstance.OnConnectionChanged then GameInstance.OnConnectionChanged(bConnected) end
    end)

    -- 规则：启动即设备登录（游客态）；登录成功后由大厅 UI 调 Lobby.EnterLobby()。
    -- 邮箱绑定（Account.BindEmail）与登录方式切换属后续 UI 里程碑，接口已就绪。
    Account.Login(function(ok, err)
        if not ok then
            print("[Account] 设备登录失败: " .. tostring(err))
            return
        end
        Account.ActivateSession(Backend, Realtime)
        print("[Account] 设备登录成功，实时通道连接中")
    end)

    print("[GameInstance] 全局初始化完成，服务层已就绪")
end

--- 取网络规则层模块集（UI/调试入口：GetNet().Lobby:SendChat("hi") 等）
function GameInstance.GetNet()
    return net
end

-- 语言已切换（C++ 已把语言落地到引擎 culture 之后派发；启动期也会推送一次当前语言）
-- 说明：引擎会自动刷新"FText 驱动的界面文案"，但 Lua 文案表与自行拼接的字符串不会，
--       故必须在这里把语言变更转给 Lua 文案层，由它通知各界面重新拉取文案。
function GameInstance:OnLanguageChanged(NewCultureCode)
    Localization.Reload(NewCultureCode)
end

-- 全局关闭前
-- 说明：本回调由 C++ 在 Super::Shutdown 之前派发，此处应完成落盘、遥测冲刷等收尾动作。
function GameInstance:OnGameInstanceShutdown()
    print("[GameInstance] 即将关闭，执行收尾")
end

return GameInstance
