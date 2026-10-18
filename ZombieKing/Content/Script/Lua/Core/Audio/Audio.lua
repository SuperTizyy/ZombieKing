-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Audio/Audio.lua
-- 模块路径: Lua.Core.Audio.Audio（UnLua 的 require 用 . 连接目录层级）
--
-- 竞技生化网游：Lua 音频入口（玩法/表现层"报事件标签"的唯一出口）
--
-- 分工（与音频框架的三层一致）：
--   * 内容/数据层（UGameAudioEventTable）：标签 → 声音资产 + 音量/音高/冷却/衰减 参数；
--   * C++ 服务层（UGameAudioSubsystem）：按标签播放、音量设置落地、混音状态切换；
--   * Lua 本文件 + 各玩法脚本：**决定"什么时候报哪个标签"**（规则/表现可热更）。
-- 因此玩法脚本里绝不出现"声音资产路径"，只出现"标签"——音频资源替换不碰玩法代码。
--
-- 使用方式（任何持有"有世界上下文"对象的 Lua 脚本）：
--   local Audio = require("Lua.Core.Audio.Audio")
--   Audio.PlayAttached(self, Tag, self.CharacterMesh, "Muzzle")   -- 挂在挂点上播（跟随发声体）
--   Audio.PlayAt(self, Tag, Location, self)                      -- 在世界位置播（爆炸点等）
--   Audio.Play2D(self, Tag)                                      -- 2D 播（UI 音 / 全局播报）
--   Audio.RequestTag("Audio.Match.ZombieWin")                    -- 按名字取标签（Lua 无法自行构造标签）
--
-- ContextObject 的作用：本模块通过引擎的 USubsystemBlueprintLibrary 取音频服务，
--   它需要一个"有世界的对象"（角色、组件、UI 控件等）来定位 GameInstance 子系统。
--   —— 这与项目既有的"取 GameInstance 层服务"约定完全一致（见 GameInstance.lua 头注释）。
-- =============================================================================

local Audio = {}

-- =============================================================================
-- 内部：取音频服务
-- =============================================================================
-- 说明：不做缓存 —— USubsystemBlueprintLibrary 内部就是一次哈希查找，开销极低；
--       而缓存服务指针会在"GameInstance 重建"（如编辑器 PIE 重进）后悬空，得不偿失。
local function GetAudioService(ContextObject)
    if ContextObject == nil then
        return nil
    end
    return UE.USubsystemBlueprintLibrary.GetGameInstanceSubsystem(ContextObject, UE.UGameAudioSubsystem)
end

-- =============================================================================
-- 标签工具（Lua 无法自行构造/比较 FGameplayTag，故统一经 C++ 提供的入口）
-- =============================================================================
--- 由标签名取标签对象（标签不存在时返回"无效标签"，不会报错）
---@param EventTagName string 标签名（如 "Audio.Weapon.Fire"）
function Audio.RequestTag(EventTagName)
    if EventTagName == nil or EventTagName == "" then
        return UE.UGameAudioSubsystem.RequestAudioEventTag("None")
    end
    return UE.UGameAudioSubsystem.RequestAudioEventTag(EventTagName)
end

--- 判断标签是否有效（空标签表示"内容侧没配这条音效"，调用方应跳过播放）
function Audio.IsValidTag(EventTag)
    if EventTag == nil then
        return false
    end
    -- 用引擎自带的蓝图库判断（IsGameplayTagValid 是 UFUNCTION，UnLua 可用）
    return UE.UBlueprintGameplayTagLibrary.IsGameplayTagValid(EventTag)
end

-- =============================================================================
-- 播放入口
-- =============================================================================
--- 在世界位置播放一条音频事件（爆炸点、弹着点等不跟随发声体的一次性音效）
---@param ContextObject any 有世界的对象（角色/组件/UI 控件）
---@param EventTag FGameplayTag 音频事件标签
---@param Location FVector 世界位置
---@param InstigatorActor AActor 触发者（可空）
function Audio.PlayAt(ContextObject, EventTag, Location, InstigatorActor)
    local Service = GetAudioService(ContextObject)
    if Service == nil or Audio.IsValidTag(EventTag) == false or Location == nil then
        return
    end
    Service:PlayAudioEventAtLocation(EventTag, Location, InstigatorActor)
end

--- 把音频事件挂在某个场景组件上播放（会跟随组件移动；枪口、脚步、换弹等"发声体自身"的音效）
---@param AttachToComponent USceneComponent 挂接组件（须有效）
---@param AttachSocketName string 挂点名（可空 = 组件原点）
function Audio.PlayAttached(ContextObject, EventTag, AttachToComponent, AttachSocketName)
    local Service = GetAudioService(ContextObject)
    if Service == nil or Audio.IsValidTag(EventTag) == false or AttachToComponent == nil then
        return
    end
    -- FName 形参：nil 会让跨语言转换失败，故统一回退为 "None"（即 NAME_None）
    Service:PlayAudioEventAttached(EventTag, AttachToComponent, AttachSocketName or "None")
end

--- 播放 2D 音频事件（UI 音、全局播报；不受空间衰减影响）
function Audio.Play2D(ContextObject, EventTag)
    local Service = GetAudioService(ContextObject)
    if Service == nil or Audio.IsValidTag(EventTag) == false then
        return
    end
    Service:PlayAudioEvent2D(EventTag)
end

-- =============================================================================
-- 混音状态（整段氛围切换：菜单/对局/观战）
-- =============================================================================
--- 切换混音状态（默认已由 C++ 按前端流程自动切换；Lua 用它覆盖特殊状态，如进入观战）
---@param StateName string 状态名："Boot" / "Menu" / "Match" / "Spectate"
function Audio.SetState(ContextObject, StateName)
    local Service = GetAudioService(ContextObject)
    if Service == nil then
        return
    end

    -- Lua 传入的是字符串，这里映射为 C++ 的 UENUM 值（UnLua 约定：UE.<枚举类型>.<枚举名>）
    local StateMap =
    {
        Boot     = UE.EGameAudioState.Boot,
        Menu     = UE.EGameAudioState.Menu,
        Match    = UE.EGameAudioState.Match,
        Spectate = UE.EGameAudioState.Spectate,
    }

    local State = StateMap[StateName]
    if State == nil then
        print(string.format("[音频] 未知的混音状态名：%s", tostring(StateName)))
        return
    end

    Service:SetAudioState(State)
end

return Audio
