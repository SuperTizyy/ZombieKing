-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Localization/Localization.lua
-- 模块路径: Lua.Core.Localization.Localization（UnLua 的 require 用 . 连接目录层级）
--
-- 竞技生化网游：Lua 文案层（玩法文案 / 提示 / 播报 / 结算等"需要热更"的文本）
--
-- 双层本地化分工（用户已拍板）：
--   * **UI 文案**（界面控件的固定文案）→ UE 本地化表（UStringTable）+ FText：
--     经 C++ UGameLocalizationSubsystem:GetUIText(TextId) 取，可被引擎本地化管线收集、打包 LocRes，
--     文化切换后由引擎自动重绘刷新（Slate 会订阅文本版本变化并 InvalidateAllWidgets）；
--   * **玩法文案**（技能名、命中提示、结算播报、GM 文本等）→ **本文件**：
--     改文案只改本文件即刻生效，无需重编译 C++、无需重新打包 LocRes（竞技网游不停机热更的硬要求）。
--
-- 文本 ID 规范（用户已拍板：分层点分 ID）
--   形如「域.模块.键」，全项目统一、Lua 与 UI 文案表可共用同一套 ID：
--     UI.Menu.StartGame            —— 域=UI，模块=Menu，键=StartGame
--     UI.Common.Confirm            —— 通用词汇放 Common 模块，供各界面复用
--     Match.Phase.Lobby            —— 对局阶段名
--     Match.Result.HumanWin        —— 结算播报
--     Ability.Zombie.Claw.Name     —— 技能名（按阵营/技能分层）
--   纪律：**同一 ID 必须成对存在于基准语言与各语种表中**（缺一边按下面的回退链处理）。
--
-- 缺失回退链（用户已拍板：回退基准语言）
--   当前语言缺 → 基准语言(NATIVE_CULTURE)缺 → **返回文本 ID 本身** + 一次性告警。
--   刻意不返回 nil / 空串：界面出现"神秘空白"比显示一个可定位的 ID 更难排障。
--
-- 使用方式（任何 Lua 脚本）：
--   local Localization = require("Lua.Core.Localization.Localization")
--   local Text = Localization.GetText("UI.Common.Confirm")            -- 取文案
--   local Text = Localization.Format("UI.Toast.Killed", 3)            -- 带占位符 {0}/{1}...
--   Localization.AddListener(function(Culture) ... end)               -- 订阅语言变更（刷新界面文案）
-- =============================================================================

local Localization = {}

-- =============================================================================
-- 基准（Native）语言：文案的撰写语言，也是缺失时的回退目标。
-- 与 C++ 的 UGameLocalizationSettings::NativeCulture 保持一致（C++ 侧为权威配置，
-- 此处冗余一份是为了让"纯 Lua 路径"在未取到 C++ 服务时也能独立工作）。
-- =============================================================================
local NATIVE_CULTURE = "zh-Hans"

-- =============================================================================
-- Lua 文案表（可热更）：culture → { [TextId] = 文案 }
--   热更说明：线上调整文案 = 只改本表内容 → 生效，无需重编译、无需重新打包。
--   扩语种：加一份 _TEXT_TABLE["<culture>"] 子表即可（同时应在 C++ 配置里登记该语言，
--           否则它不会出现在语言选择列表里、也不会被写入玩家设置）。
--
--   注：下面这份是**起始种子**（用于验证管线与展示 ID 规范），真实文案由内容侧持续补齐。
-- =============================================================================
local _TEXT_TABLE =
{
    ["zh-Hans"] =
    {
        -- 通用词汇
        ["UI.Common.Confirm"]        = "确定",
        ["UI.Common.Cancel"]         = "取消",
        ["UI.Common.Apply"]          = "应用",

        -- 主菜单
        ["UI.Menu.StartGame"]        = "开始游戏",
        ["UI.Menu.Settings"]         = "设置",
        ["UI.Menu.Quit"]             = "退出游戏",

        -- 设置：语言
        ["UI.Settings.Language"]     = "语言",
        ["UI.Settings.LanguageHint"] = "对局中无法切换语言，请返回大厅后再修改。",

        -- 对局阶段
        ["Match.Phase.Lobby"]        = "等待开局",
        ["Match.Phase.Match"]        = "对局进行中",

        -- 结算播报
        ["Match.Result.HumanWin"]    = "人类阵营获胜",
        ["Match.Result.ZombieWin"]   = "生化幽灵获胜",

        -- 技能名
        ["Ability.Zombie.Claw.Name"] = "撕裂之爪",
    },

    ["en"] =
    {
        ["UI.Common.Confirm"]        = "Confirm",
        ["UI.Common.Cancel"]         = "Cancel",
        ["UI.Common.Apply"]          = "Apply",

        ["UI.Menu.StartGame"]        = "Play",
        ["UI.Menu.Settings"]         = "Settings",
        ["UI.Menu.Quit"]             = "Quit",

        ["UI.Settings.Language"]     = "Language",
        ["UI.Settings.LanguageHint"] = "Language cannot be changed during a match. Return to the lobby first.",

        ["Match.Phase.Lobby"]        = "Waiting",
        ["Match.Phase.Match"]        = "In Progress",

        ["Match.Result.HumanWin"]    = "Humans Win",
        ["Match.Result.ZombieWin"]   = "Zombies Win",

        ["Ability.Zombie.Claw.Name"] = "Rending Claw",
    },
}

-- =============================================================================
-- 模块内部状态
-- =============================================================================
-- 当前生效语言（由 C++ 通过 GameInstance 事件推送后写入；初值取基准语言，
-- 保证"C++ 尚未推送"的极端情况下依然有可读文案）
local _CurrentCulture = NATIVE_CULTURE

-- 已告警过的缺失 ID（只告警一次：缺失文案常出现在高频刷新的界面上，逐次告警会淹掉日志）
local _WarnedIds = {}

-- 语言变更听众（界面脚本 / 流程脚本注册；语言变化后逐个回调，让它们重新拉取文案）
local _Listeners = {}

-- =============================================================================
-- 生命周期与语言变更
-- =============================================================================
--- 初始化（由 GameInstance 脚本在 OnGameInstanceInit 中调用一次）
function Localization.Init()
    -- 清空"已告警 ID"与"语言变更听众"：避免上一次会话 / 热重载后残留
    _WarnedIds = {}
    _Listeners = {}

    -- 注意：**刻意不重置 _CurrentCulture**。
    -- 原因：启动期语言可能在 Init 之前就已由 C++ 推送（若引擎初始语言与玩家保存语言不同，
    --       C++ 会在服务装配阶段就广播一次），此处重置会把已经正确的语言打回基准语言；
    --       而模块加载期的初值本身就是基准语言，因此"不重置"在两种时序下都是正确且稳定的。
    print("[本地化] Lua 文案层已初始化（当前语言 " .. _CurrentCulture .. "，基准语言 " .. NATIVE_CULTURE .. "）")
end

--- 语言已切换（由 GameInstance 脚本在 C++ 的 OnLanguageChanged 事件里转发进来）
--- 说明：C++ 已把语言落地到引擎 culture 并广播，本函数只负责"Lua 侧相关动作"：
---       ① 更新当前语言；② 通知听众重新拉取文案（界面脚本在此刷新自己的文本控件）。
---@param NewCultureCode string 切换后的语言代码（如 "zh-Hans" / "en"）
function Localization.Reload(NewCultureCode)
    if NewCultureCode == nil or NewCultureCode == "" then
        return
    end
    if NewCultureCode == _CurrentCulture then
        return
    end

    local OldCulture = _CurrentCulture
    _CurrentCulture = NewCultureCode
    print(string.format("[本地化] Lua 文案已切换：%s -> %s", OldCulture, _CurrentCulture))

    -- 先做快照再回调：听众可能在回调里注销自己，直接遍历原表会造成漏调/错调
    local Snapshot = {}
    for Index, Listener in ipairs(_Listeners) do
        Snapshot[Index] = Listener
    end

    for _, Listener in ipairs(Snapshot) do
        -- 用 pcall 隔离：单个界面刷新报错不应打断其它界面的刷新（线上排障也更好定位）
        local Ok, Error = pcall(Listener, _CurrentCulture)
        if not Ok then
            print(string.format("[本地化] 语言变更听众执行失败：%s", tostring(Error)))
        end
    end
end

--- 取当前生效语言代码
function Localization.GetCurrentCulture()
    return _CurrentCulture
end

--- 取基准（回退）语言代码
function Localization.GetNativeCulture()
    return NATIVE_CULTURE
end

--- 注册语言变更听众（界面脚本用它刷新自己的文案）
---@param Listener function 形如 function(NewCultureCode) ... end
function Localization.AddListener(Listener)
    if type(Listener) ~= "function" then
        print("[本地化] AddListener 需要传入函数")
        return
    end
    _Listeners[#_Listeners + 1] = Listener
end

--- 注销语言变更听众（界面关闭时应调用，避免已销毁界面被回调）
function Localization.RemoveListener(Listener)
    for Index = #_Listeners, 1, -1 do
        if _Listeners[Index] == Listener then
            table.remove(_Listeners, Index)
        end
    end
end

-- =============================================================================
-- 文案查询
-- =============================================================================
--- 按文本 ID 取文案（缺失时按"当前语言 → 基准语言 → 显示 ID 本身"回退）
---@param TextId string 分层点分文本 ID（如 "UI.Menu.StartGame"）
---@return string 文案（永不返回 nil / 空串，保证界面不会出现"神秘空白"）
function Localization.GetText(TextId)
    if TextId == nil or TextId == "" then
        return ""
    end

    -- ① 当前语言
    local CurrentTable = _TEXT_TABLE[_CurrentCulture]
    local Text = CurrentTable and CurrentTable[TextId]
    if Text then
        return Text
    end

    -- ② 回退基准语言（用户拍板的回退策略）
    if _CurrentCulture ~= NATIVE_CULTURE then
        local NativeTable = _TEXT_TABLE[NATIVE_CULTURE]
        Text = NativeTable and NativeTable[TextId]
        if Text then
            return Text
        end
    end

    -- ③ 两份表都缺：返回 ID 本身 + 一次性告警（可读、可定位，便于内容侧补齐）
    if not _WarnedIds[TextId] then
        _WarnedIds[TextId] = true
        print(string.format("[本地化] Lua 文案 ID 缺失：%s（当前语言 %s，已回退为显示 ID 本身）", TextId, _CurrentCulture))
    end
    return TextId
end

--- 取带参数的文案：占位符写法与 UE 的 FText 参数保持一致（{0} / {1} ...）
--- 目的：让"UI 文案（UE 表）"与"玩法文案（Lua 表）"共用同一种占位符写法，策划对表时不必切换心智。
---@param TextId string 文本 ID
---@return string 已替换占位符的文案
function Localization.Format(TextId, ...)
    local Template = Localization.GetText(TextId)
    local Args = { ... }

    -- 注意：Lua 里 { 与 } 不是模式特殊字符，可直接参与匹配
    local Result = string.gsub(Template, "{(%d+)}", function(IndexText)
        local Value = Args[tonumber(IndexText) + 1]   -- {0} 对应第 1 个参数
        return Value ~= nil and tostring(Value) or ""
    end)

    return Result
end

return Localization
