---LuaPanda 调试引导模块
---将"调试环境准备 + 启动调试器"集中在本文件, 启动入口(Bootstrap.lua)只需 require 本模块。
---
---为什么需要这些准备(踩坑记录):
---  1. UnLua 内嵌 Lua 的 loadlib 以 LoadLibraryEx(path, NULL, 0) 标准模式加载 DLL,
---     依赖库不会从 core.dll 所在目录搜索; 必须先显式预加载 lua54.dll,
---     否则报 "找不到指定的模块", LuaPanda 表现为 reGetSock fail。
---  2. UnLua 嵌入环境下 package.cpath 默认不含 Content/Script;
---     且 UE5.6 的 ProjectContentDir 返回相对路径, 必须转为绝对路径后再追加。
---  3. 启动前自检 require("socket.core"), 失败时打印真实错误(LuaPanda 内部会吞错)。
---注意: Content/Script/socket 目录下必须保留 lua54.dll(勿删)。
---
---用法: 在游戏启动脚本中 require("Lua.Core.DebugBootstrap")

local M = {}

-- 计算 Content 目录(绝对路径)
local ok, ContentDir = pcall(function()
    local dir = UE.UBlueprintPathsLibrary.ProjectContentDir()
    if not dir or dir == "" then return nil end
    -- 转成绝对路径(失败时退回原路径)
    local okFull, full = pcall(function()
        return UE.UBlueprintPathsLibrary.ConvertRelativePathToFull(dir)
    end)
    if okFull and full and full ~= "" then return full end
    return dir
end)

if ok and type(ContentDir) == "string" and ContentDir ~= "" then
    -- 第1步: 预加载 lua54.dll("*" 是 Lua 官方"仅加载"标志)
    local preOK = false
    if package.loadlib then
        local preRet, preMsg = select(2, pcall(package.loadlib, ContentDir .. "Script/socket/lua54.dll", "*"))
        preOK = (preRet == true)
        if not preOK then
            print("[LuaPanda] 预加载 lua54.dll 失败: " .. tostring(preMsg))
        end
    else
        print("[LuaPanda] 警告: package.loadlib 不可用, 无法预加载 lua54.dll")
    end

    -- 第2步: 追加 C 模块搜索路径
    package.cpath = package.cpath .. ";" .. ContentDir .. "Script/?.dll"

    -- 第3步: 自检(成功仅一行日志, 失败输出完整诊断)
    local sockOK, sockErr = pcall(require, "socket.core")
    if sockOK then
        print("[LuaPanda] 调试环境就绪")
    else
        print("[LuaPanda] 自检: socket.core 加载失败 -> " .. tostring(sockErr))
        print("[LuaPanda] 预加载 lua54.dll: " .. tostring(preOK))
        print("[LuaPanda] 当前 package.cpath: " .. package.cpath)
    end
else
    print("[LuaPanda] 警告: 获取 Content 目录失败, luasocket 可能无法加载! " .. tostring(ContentDir))
end

-- 启动调试器(VSCode 未启动调试时按 attach 模式每秒自动重试)
require("LuaPanda").start("127.0.0.1", 8818)

return M
