// json for mods, written in Lua: json.encode(value) and json.decode(text). Tables with keys 1..n are arrays (an empty
// table is []), tables with string keys are objects, written with sorted keys so saved files diff cleanly. null
// decodes to nil. Errors are plain Lua errors, so a mod can pcall them.
// Limit: output is compact, not pretty-printed; a json.encode(v, true) option if mods need hand-editable files.
#include "lua_api.h"

#include "lauxlib.h"
#include "lua.h"

namespace {

const char kJson[] = R"lua(
local concat, format, sort, utf8char, mathtype = table.concat, string.format, table.sort, utf8.char, math.type
local error, pairs, ipairs, type, tonumber, tostring = error, pairs, ipairs, type, tonumber, tostring
local json = {}

local escapes = {['"'] = '\\"', ['\\'] = '\\\\', ['\b'] = '\\b', ['\f'] = '\\f', ['\n'] = '\\n', ['\r'] = '\\r',
                 ['\t'] = '\\t'}

local function str(s)
    return '"' .. s:gsub('[%c"\\]', function(c) return escapes[c] or format("\\u%04x", c:byte()) end) .. '"'
end

local function num(n)
    if n ~= n or n == math.huge or n == -math.huge then error("json can't encode " .. tostring(n), 0) end
    if mathtype(n) == "integer" then return format("%d", n) end
    local s = format("%.14g", n)
    if tonumber(s) ~= n then s = format("%.17g", n) end
    return s
end

local encode

local function encodeTable(t, seen)
    if seen[t] then error("json can't encode a table that contains itself", 0) end
    seen[t] = true
    local n, ints, keys, out = #t, 0, {}, {}
    for k in pairs(t) do
        if type(k) == "string" then
            keys[#keys + 1] = k
        elseif mathtype(k) == "integer" and k >= 1 and k <= n then
            ints = ints + 1
        else
            error("json can't encode a table with key " .. tostring(k), 0)
        end
    end
    if ints ~= n or (n > 0 and #keys > 0) then error("json can't encode a sparse or mixed table", 0) end
    if #keys == 0 then
        for i = 1, n do out[i] = encode(t[i], seen) end
    else
        sort(keys)
        for i, k in ipairs(keys) do out[i] = str(k) .. ":" .. encode(t[k], seen) end
    end
    seen[t] = nil
    return #keys == 0 and "[" .. concat(out, ",") .. "]" or "{" .. concat(out, ",") .. "}"
end

encode = function(v, seen)
    local t = type(v)
    if t == "nil" then return "null" end
    if t == "boolean" then return tostring(v) end
    if t == "number" then return num(v) end
    if t == "string" then return str(v) end
    if t == "table" then return encodeTable(v, seen) end
    error("json can't encode a " .. t, 0)
end

function json.encode(v) return encode(v, {}) end

local function fail(i, what) error(format("json: %s at position %d", what, i), 0) end
local function skip(s, i) return s:find("[^ \t\r\n]", i) or #s + 1 end

local unescapes = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}

-- \uXXXX at i, with a following low surrogate if it is a high one: the code point and where the escape ends.
local function codepoint(s, i)
    local hex = s:match("^%x%x%x%x", i + 2)
    if not hex then fail(i, "bad \\u escape") end
    local cp = tonumber(hex, 16)
    if cp < 0xD800 or cp > 0xDBFF then return cp, i + 6 end
    local lo = s:match("^\\u(%x%x%x%x)", i + 6)
    lo = lo and tonumber(lo, 16)
    if not lo or lo < 0xDC00 or lo > 0xDFFF then fail(i, "bad surrogate pair") end
    return 0x10000 + (cp - 0xD800) * 0x400 + (lo - 0xDC00), i + 12
end

local function decodeString(s, i)  -- i is at the opening quote
    local out, j = {}, i + 1
    while true do
        local k = s:find('[%c"\\]', j)
        if not k then fail(i, "unterminated string") end
        out[#out + 1] = s:sub(j, k - 1)
        local c = s:sub(k, k)
        if c == '"' then return concat(out), k + 1 end
        if c ~= "\\" then fail(k, "control character in string") end
        local e = s:sub(k + 1, k + 1)
        if e == "u" then
            local cp
            cp, j = codepoint(s, k)
            out[#out + 1] = utf8char(cp)
        else
            if not unescapes[e] then fail(k, "bad escape") end
            out[#out + 1] = unescapes[e]
            j = k + 2
        end
    end
end

local function decodeNumber(s, i)
    local text = s:match("^-?%d+%.?%d*", i)
    if not text then fail(i, "unexpected character") end
    text = text .. (s:match("^[eE][-+]?%d+", i + #text) or "")
    return tonumber(text), i + #text
end

local decodeValue

local function decodeArray(s, i)
    local t, n = {}, 0
    i = skip(s, i + 1)
    if s:sub(i, i) == "]" then return t, i + 1 end
    while true do
        n = n + 1
        t[n], i = decodeValue(s, i)
        i = skip(s, i)
        local c = s:sub(i, i)
        if c == "]" then return t, i + 1 end
        if c ~= "," then fail(i, "expected , or ]") end
        i = i + 1
    end
end

local function decodeObject(s, i)
    local t = {}
    i = skip(s, i + 1)
    if s:sub(i, i) == "}" then return t, i + 1 end
    while true do
        if s:sub(i, i) ~= '"' then fail(i, "expected a string key") end
        local k
        k, i = decodeString(s, i)
        i = skip(s, i)
        if s:sub(i, i) ~= ":" then fail(i, "expected :") end
        t[k], i = decodeValue(s, i + 1)
        i = skip(s, i)
        local c = s:sub(i, i)
        if c == "}" then return t, i + 1 end
        if c ~= "," then fail(i, "expected , or }") end
        i = skip(s, i + 1)
    end
end

local literals = {{"true", true}, {"false", false}, {"null", nil}}

decodeValue = function(s, i)
    i = skip(s, i)
    local c = s:sub(i, i)
    if c == "{" then return decodeObject(s, i) end
    if c == "[" then return decodeArray(s, i) end
    if c == '"' then return decodeString(s, i) end
    for _, lit in ipairs(literals) do
        if s:sub(i, i + #lit[1] - 1) == lit[1] then return lit[2], i + #lit[1] end
    end
    if c == "" then fail(i, "unexpected end") end
    return decodeNumber(s, i)
end

function json.decode(s)
    if type(s) ~= "string" then error("bad argument #1 to 'decode' (string expected)", 2) end
    local v, i = decodeValue(s, 1)
    i = skip(s, i)
    if i <= #s then fail(i, "unexpected text after the value") end
    return v
end

return json
)lua";

}  // namespace

void rt::openJson(lua_State* L) {
    luaL_loadbuffer(L, kJson, sizeof kJson - 1, "=json");
    lua_call(L, 0, 1);
    lua_setglobal(L, "json");
}
