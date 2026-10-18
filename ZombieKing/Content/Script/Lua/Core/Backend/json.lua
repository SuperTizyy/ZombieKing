-- =============================================================================
-- 文件路径: Content/Script/Lua/Core/Backend/json.lua
-- 模块路径: Lua.Core.Backend.json
--
-- 纯 Lua JSON 编解码（自实现，Lua 5.4 兼容；仅覆盖后端报文所需的
-- object/array/string/number/boolean/null 类型，不支持注释与多根值）
-- 用途：RPC 业务体与 rtapi Envelope 的序列化/反序列化。
-- 纪律：仅做数据格式转换，不含任何业务规则。
-- =============================================================================

local json = {}

-- ---------------------------------------------------------------------------
-- encode
-- ---------------------------------------------------------------------------

local encode_val

local function is_array(t)
    local n = 0
    for k in pairs(t) do
        n = n + 1
        if t[n] == nil then return false end
    end
    return n > 0
end

local escape_cache = {}

local function esc_char(c)
    local mapped = escape_cache[c]
    if mapped then return mapped end
    if c == '"' then mapped = '\\"'
    elseif c == '\\' then mapped = '\\\\'
    elseif c == '\b' then mapped = '\\b'
    elseif c == '\f' then mapped = '\\f'
    elseif c == '\n' then mapped = '\\n'
    elseif c == '\r' then mapped = '\\r'
    elseif c == '\t' then mapped = '\\t'
    else mapped = c end
    escape_cache[c] = mapped
    return mapped
end

local function encode_string(s)
    return '"' .. s:gsub('[%c"\\]', esc_char) .. '"'
end

local function encode_number(v)
    if v ~= v or v == math.huge or v == -math.huge then
        return "null" -- 非法数值按 JSON 规范落 null
    end
    if math.floor(v) == v and math.abs(v) < 1e15 then
        return string.format("%d", v)
    end
    return string.format("%.14g", v)
end

encode_val = function(v)
    local t = type(v)
    if t == "string" then
        return encode_string(v)
    elseif t == "number" then
        return encode_number(v)
    elseif t == "boolean" then
        return v and "true" or "false"
    elseif v == nil then
        return "null"
    elseif t == "table" then
        if is_array(v) then
            local parts = {}
            for i = 1, #v do
                parts[i] = encode_val(v[i])
            end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            local keys = {}
            for k in pairs(v) do
                if type(k) == "string" then keys[#keys + 1] = k end
            end
            table.sort(keys)
            local parts = {}
            for i, k in ipairs(keys) do
                parts[i] = encode_string(k) .. ":" .. encode_val(v[k])
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return "null"
end

function json.encode(v)
    return encode_val(v)
end

-- ---------------------------------------------------------------------------
-- decode
-- ---------------------------------------------------------------------------

local decode_val

local function fail(pos, msg)
    error("json.decode: " .. msg .. " @" .. pos, 0)
end

local function skip_ws(s, pos)
    local c = s:sub(pos, pos)
    while c == " " or c == "\t" or c == "\n" or c == "\r" do
        pos = pos + 1
        c = s:sub(pos, pos)
    end
    return pos
end

local function decode_string(s, pos)
    if s:sub(pos, pos) ~= '"' then fail(pos, "expect string quote") end
    local parts = {}
    local i = pos + 1
    while true do
        local c = s:sub(i, i)
        if c == "" then fail(pos, "unterminated string") end
        if c == '"' then
            return table.concat(parts), i + 1
        elseif c == "\\" then
            local n = s:sub(i + 1, i + 1)
            if n == "n" then parts[#parts + 1] = "\n"
            elseif n == "t" then parts[#parts + 1] = "\t"
            elseif n == "r" then parts[#parts + 1] = "\r"
            elseif n == "b" then parts[#parts + 1] = "\b"
            elseif n == "f" then parts[#parts + 1] = "\f"
            elseif n == "/" then parts[#parts + 1] = "/"
            elseif n == '"' then parts[#parts + 1] = '"'
            elseif n == "\\" then parts[#parts + 1] = "\\"
            elseif n == "u" then
                local hex = s:sub(i + 2, i + 5)
                parts[#parts + 1] = utf8.char(tonumber(hex, 16))
                i = i + 4
            else fail(i, "bad escape \\" .. n) end
            i = i + 2
        else
            parts[#parts + 1] = c
            i = i + 1
        end
    end
end

local function decode_number(s, pos)
    local numStr = s:match("^[-+]?%d+%.?%d*[eE][-+]?%d*", pos)
    if not numStr then fail(pos, "bad number") end
    return tonumber(numStr), pos + #numStr
end

local function decode_literal(s, pos)
    if s:sub(pos, pos + 3) == "true" then return true, pos + 4 end
    if s:sub(pos, pos + 4) == "false" then return false, pos + 5 end
    if s:sub(pos, pos + 3) == "null" then return nil, pos + 4 end
    fail(pos, "bad literal")
end

decode_val = function(s, pos)
    pos = skip_ws(s, pos)
    local c = s:sub(pos, pos)
    if c == '"' then
        return decode_string(s, pos)
    elseif c == "{" then
        local obj = {}
        pos = skip_ws(s, pos + 1)
        if s:sub(pos, pos) == "}" then return obj, pos + 1 end
        while true do
            local key
            key, pos = decode_string(s, skip_ws(s, pos))
            pos = skip_ws(s, pos)
            if s:sub(pos, pos) ~= ":" then fail(pos, "expect colon") end
            local val
            val, pos = decode_val(s, pos + 1)
            obj[key] = val
            pos = skip_ws(s, pos)
            local sep = s:sub(pos, pos)
            if sep == "," then pos = pos + 1
            elseif sep == "}" then return obj, pos + 1
            else fail(pos, "expect , or }") end
        end
    elseif c == "[" then
        local arr = {}
        pos = skip_ws(s, pos + 1)
        if s:sub(pos, pos) == "]" then return arr, pos + 1 end
        while true do
            local val
            val, pos = decode_val(s, pos)
            arr[#arr + 1] = val
            pos = skip_ws(s, pos)
            local sep = s:sub(pos, pos)
            if sep == "," then pos = skip_ws(s, pos + 1)
            elseif sep == "]" then return arr, pos + 1
            else fail(pos, "expect , or ]") end
        end
    elseif c == "t" or c == "f" or c == "n" then
        return decode_literal(s, pos)
    elseif c == "-" or c == "+" or (c >= "0" and c <= "9") then
        return decode_number(s, pos)
    end
    fail(pos, "unexpected char '" .. c .. "'")
end

--- 解析 JSON 文本；非法输入返回 nil + 错误文本（不抛异常，调用方按失败分支处理）
function json.decode(s)
    if type(s) ~= "string" or s == "" then return nil, "empty" end
    local ok, val = pcall(decode_val, s, 1)
    if not ok then return nil, tostring(val) end
    return val
end

return json
