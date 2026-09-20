#include "lua_host.h"

#include <ctime>
#include <string>

#include "debug_server.h"
#include "log.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace bg3le {
namespace {

lua_State* g_lua = nullptr;

// Bound functions must outlive the closures that reference them, and the
// storage must not move once pointers are handed to Lua.
std::vector<osi::Function> g_functions;

bool to_value(lua_State* L, int idx, osi::Value* out) {
    switch (lua_type(L, idx)) {
        case LUA_TSTRING:
            out->type = osi::kString;
            out->text = lua_tostring(L, idx);
            return true;
        case LUA_TBOOLEAN:
            out->type = osi::kInteger;
            out->integer = lua_toboolean(L, idx);
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                out->type = osi::kInteger;
                out->integer = lua_tointeger(L, idx);
            } else {
                out->type = osi::kReal;
                out->real = lua_tonumber(L, idx);
            }
            return true;
        default:
            return false;
    }
}

void push_value(lua_State* L, const osi::Value& v) {
    switch (v.type) {
        case osi::kString:
        case osi::kGuidString: lua_pushstring(L, v.text.c_str()); break;
        case osi::kReal: lua_pushnumber(L, v.real); break;
        default: lua_pushinteger(L, v.integer); break;
    }
}

// Osi.Name(inputs...) -- whatever the caller omits is treated as an output,
// matching how the engine's own argument lists are shaped.
int osi_dispatch(lua_State* L) {
    const auto* fn = static_cast<const osi::Function*>(
        lua_touserdata(L, lua_upvalueindex(1)));
    const int argc = lua_gettop(L);

    if (static_cast<std::size_t>(argc) > fn->params.size()) {
        return luaL_error(L, "Osi.%s takes at most %d argument(s), got %d",
                          fn->name.c_str(), (int)fn->params.size(), argc);
    }

    std::vector<osi::Value> inputs;
    inputs.reserve(argc);
    for (int i = 1; i <= argc; ++i) {
        osi::Value v;
        if (!to_value(L, i, &v)) {
            return luaL_error(L, "Osi.%s: argument %d has unsupported type %s",
                              fn->name.c_str(), i, luaL_typename(L, i));
        }
        inputs.push_back(std::move(v));
    }

    std::vector<osi::Value> outputs;
    if (!osi::invoke(*fn, inputs, &outputs)) {
        lua_pushnil(L);
        return 1;
    }
    for (const osi::Value& v : outputs) push_value(L, v);
    return static_cast<int>(outputs.size());
}

// Output goes to the attached debugger client as well as the log, which is
// what makes the remote prompt useful. Severity rides in an upvalue so
// Ext.Log.Print/PrintWarning/PrintError share one implementation.
int l_log(lua_State* L) {
    const int severity = static_cast<int>(lua_tointeger(L, lua_upvalueindex(1)));
    std::string out;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) out += "\t";
        out += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    debug_server_output(out.c_str(), severity);
    logf("lua: %s", out.c_str());
    return 0;
}

int l_monotonic_ms(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    return 1;
}

int l_microsec(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    lua_pushinteger(L, (lua_Integer)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
    return 1;
}

int l_clock_time(lua_State* L) {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    lua_pushnumber(L, (double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
    return 1;
}

void register_log(lua_State* L, const char* name, int severity) {
    lua_pushinteger(L, severity);
    lua_pushcclosure(L, l_log, 1);
    lua_setfield(L, -2, name);
}

}  // namespace

void lua_init() {
    if (g_lua != nullptr) return;

    g_lua = luaL_newstate();
    if (g_lua == nullptr) {
        logf("lua: luaL_newstate failed");
        return;
    }
    luaL_openlibs(g_lua);
    // Ext.Log and Ext.Json are pure Lua/C and need no engine reflection, so
    // the helpers mods actually use every day can be compatible now. Shapes
    // and aliases follow BG3SE's BuiltinLibrary.lua.
    lua_newtable(g_lua);                       // Ext
    lua_newtable(g_lua);                       // Ext.Log
    register_log(g_lua, "Print", 0);
    register_log(g_lua, "PrintWarning", 1);
    register_log(g_lua, "PrintError", 2);
    lua_setfield(g_lua, -2, "Log");

    lua_newtable(g_lua);                       // Ext.Timer
    lua_pushcfunction(g_lua, l_monotonic_ms);
    lua_setfield(g_lua, -2, "MonotonicTime");
    lua_pushcfunction(g_lua, l_microsec);
    lua_setfield(g_lua, -2, "MicrosecTime");
    lua_pushcfunction(g_lua, l_clock_time);
    lua_setfield(g_lua, -2, "ClockTime");
    lua_setfield(g_lua, -2, "Timer");

    lua_setglobal(g_lua, "Ext");

    static const char kPrelude[] = R"LUA(
-- _D is Ext.Json.Stringify, which is why strings come back quoted:
-- _D(GetHostCharacter()) yields "<uuid>" while _P yields <uuid>.
Ext.Json = Ext.Json or {}

local function encode(v, indent, depth, opts, seen, out)
  local t = type(v)
  if v == nil then out[#out+1] = "null"
  elseif t == "boolean" then out[#out+1] = tostring(v)
  elseif t == "number" then
    out[#out+1] = (math.type(v) == "integer") and tostring(v)
                  or string.format("%.14g", v)
  elseif t == "string" then
    out[#out+1] = string.format("%q", v):gsub("\\\n", "\\n")
  elseif t ~= "table" then
    out[#out+1] = string.format("%q", tostring(v))
  else
    if seen[v] then out[#out+1] = "\"<recursion>\"" return end
    if opts.LimitDepth and depth > opts.LimitDepth then
      out[#out+1] = "\"<...>\"" return
    end
    seen[v] = true

    local n = 0
    local array = true
    for k in pairs(v) do
      n = n + 1
      if type(k) ~= "number" then array = false end
    end
    array = array and n == #v

    local pad = indent .. "    "
    if n == 0 then
      out[#out+1] = array and "[]" or "{}"
    elseif array then
      out[#out+1] = "[\n"
      for i = 1, #v do
        out[#out+1] = pad
        encode(v[i], pad, depth + 1, opts, seen, out)
        out[#out+1] = (i < #v) and ",\n" or "\n"
      end
      out[#out+1] = indent .. "]"
    else
      local keys = {}
      for k in pairs(v) do keys[#keys+1] = k end
      table.sort(keys, function(a, b) return tostring(a) < tostring(b) end)
      out[#out+1] = "{\n"
      for i, k in ipairs(keys) do
        out[#out+1] = pad .. string.format("%q", tostring(k)) .. ": "
        encode(v[k], pad, depth + 1, opts, seen, out)
        out[#out+1] = (i < #keys) and ",\n" or "\n"
      end
      out[#out+1] = indent .. "}"
    end
    seen[v] = nil
  end
end

function Ext.Json.Stringify(v, opts)
  local out = {}
  encode(v, "", 1, opts or {}, {}, out)
  return table.concat(out)
end

function Ext.DumpExport(v) return Ext.Json.Stringify(v, {Beautify = true}) end
function Ext.Dump(v) Ext.Log.Print(Ext.DumpExport(v)) end
function Ext.DumpShallow(v)
  Ext.Log.Print(Ext.Json.Stringify(v, {Beautify = true, LimitDepth = 1}))
end

-- Modules needing engine reflection are stubbed so a mod gets a specific
-- error instead of "attempt to index a nil value".
local function stub_index(name)
  return function(_, key)
    return function()
      error(string.format("bg3le: Ext.%s.%s is not implemented yet", name, key), 0)
    end
  end
end

local function stub(name)
  return setmetatable({}, {__index = stub_index(name)})
end

Ext.Table = {
  Find = function(tbl, value)
    for k, v in pairs(tbl) do
      if v == value then return v, k end
    end
    return nil
  end
}
table.find = Ext.Table.Find

-- The scalar half of Ext.Math; the vector and matrix entries need their
-- userdata types, so they stub out rather than silently misbehave.
Ext.Math = setmetatable({
  Round = function(x) return math.floor(x + 0.5) end,
  Trunc = function(x) return x >= 0 and math.floor(x) or math.ceil(x) end,
  Fract = function(x) return x - math.floor(x) end,
  Sign = function(x) return (x > 0 and 1) or (x < 0 and -1) or 0 end,
  Clamp = function(x, lo, hi) return math.max(lo, math.min(hi, x)) end,
  Lerp = function(a, b, t) return a + (b - a) * t end,
  Smoothstep = function(a, b, x)
    local t = math.max(0, math.min(1, (x - a) / (b - a)))
    return t * t * (3 - 2 * t)
  end,
  IsNaN = function(x) return x ~= x end,
  IsInf = function(x) return x == math.huge or x == -math.huge end,
  Acos = math.acos, Asin = math.asin, Atan = math.atan,
  Random = function(a, b)
    if a == nil then return math.random() end
    if b == nil then return math.random(a) end
    return math.random(a, b)
  end
}, {__index = stub_index("Math")})

Ext.Utils = {
  Print = Ext.Log.Print,
  PrintWarning = Ext.Log.PrintWarning,
  PrintError = Ext.Log.PrintError,
  Round = Ext.Math.Round,
  Random = Ext.Math.Random,
  MonotonicTime = Ext.Timer.MonotonicTime,
  MicrosecTime = Ext.Timer.MicrosecTime,
}

function Ext.IsServer() return true end
function Ext.IsClient() return false end

for _, name in ipairs({"Entity", "Stats", "Level", "StaticData", "Mod", "Net",
                       "Vars", "IO", "Types", "Localization", "Debug",
                       "Events", "Resource", "Template"}) do
  Ext[name] = stub(name)
end
Ext.Definition = Ext.StaticData
Mods = {}

-- Osi cannot be bound until a story is loaded (the engine generates its
-- function table on demand), so until then explain the situation rather
-- than letting every Osiris name look like a typo.
Osi = setmetatable({}, {__index = function(_, key)
  error(string.format(
    "bg3le: Osiris is not bound yet (no story loaded), so Osi.%s is "
    .. "unavailable -- load a save first", key), 0)
end})

setmetatable(_G, {__index = function(_, key)
  error(string.format(
    "bg3le: '%s' is not defined. Osiris functions become available as "
    .. "globals once a save is loaded.", key), 0)
end})

_D = Ext.Dump
_DS = Ext.DumpShallow
_P = Ext.Log.Print
_PW = Ext.Log.PrintWarning
_PE = Ext.Log.PrintError
Print = Ext.Log.Print
print = Ext.Log.Print
)LUA";
    if (luaL_dostring(g_lua, kPrelude) != LUA_OK) {
        logf("lua: prelude failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
    logf("lua: %s up", LUA_RELEASE);
}

void lua_bind_osi(const std::vector<osi::Function>& functions) {
    if (g_lua == nullptr) return;

    g_functions = functions;  // one copy, then never resized again

    lua_createtable(g_lua, 0, static_cast<int>(g_functions.size()));
    int bound = 0;
    int events = 0;
    for (osi::Function& fn : g_functions) {
        if (fn.kind() == osi::kEvent) {
            ++events;  // raised by the game, not callable
            continue;
        }
        lua_pushlightuserdata(g_lua, &fn);
        lua_pushcclosure(g_lua, osi_dispatch, 1);
        lua_setfield(g_lua, -2, fn.name.c_str());
        ++bound;
    }
    lua_setglobal(g_lua, "Osi");

    // The Windows extender generates "Name = Osi.Name" for every symbol, so
    // mods call Osiris functions bare: _D(GetHostCharacter()) is idiomatic.
    // Matching that is the point of sharing the API surface. Verified against
    // the enumerated names that none collide with a Lua global.
    lua_getglobal(g_lua, "Osi");
    for (osi::Function& fn : g_functions) {
        if (fn.kind() == osi::kEvent) continue;
        lua_getfield(g_lua, -1, fn.name.c_str());
        lua_setglobal(g_lua, fn.name.c_str());
    }
    lua_pop(g_lua, 1);

    // Match bg3se's name resolver: a wrong-case lookup on Osi resolves with a
    // compatibility warning rather than failing, since mods rely on that
    // leniency. Globals stay exact-case, as they are there too.
    lua_run(R"LUA(
setmetatable(_G, nil)  -- Osiris is bound; typos are plain nils again

local lower = {}
for name in pairs(Osi) do lower[string.lower(name)] = name end
setmetatable(Osi, {
  __index = function(t, key)
    local real = lower[string.lower(key)]
    if real == nil then return nil end
    Ext.Log.PrintWarning(string.format(
      "COMPATIBILITY WARNING: Osiris symbol '%s' referenced using incorrect " ..
      "case; the correct name is '%s'", key, real))
    local fn = rawget(t, real)
    rawset(t, key, fn)  -- cache, so the warning fires once per name
    return fn
  end
})
)LUA");

    logf("lua: bound %d Osi functions as Osi.* and globals (%d events skipped)",
         bound, events);
}

void lua_eval(const char* code, std::string* result, std::string* error) {
    if (g_lua == nullptr) {
        *error = "Lua is not initialised";
        return;
    }

    // Prefer expression form so a bare expression yields its value, falling
    // back to statement form when that will not compile.
    const std::string as_expr = std::string("return ") + code;
    if (luaL_loadstring(g_lua, as_expr.c_str()) != LUA_OK) {
        lua_pop(g_lua, 1);
        if (luaL_loadstring(g_lua, code) != LUA_OK) {
            *error = lua_tostring(g_lua, -1);
            lua_pop(g_lua, 1);
            return;
        }
    }

    const int before = lua_gettop(g_lua) - 1;
    if (lua_pcall(g_lua, 0, LUA_MULTRET, 0) != LUA_OK) {
        *error = lua_tostring(g_lua, -1);
        lua_pop(g_lua, 1);
        return;
    }

    const int count = lua_gettop(g_lua) - before;
    for (int i = 0; i < count; ++i) {
        if (i > 0) *result += "\t";
        *result += luaL_tolstring(g_lua, before + 1 + i, nullptr);
        lua_pop(g_lua, 1);
    }
    lua_pop(g_lua, count);
}

void lua_run(const char* code) {
    if (g_lua == nullptr) return;
    if (luaL_dostring(g_lua, code) != LUA_OK) {
        logf("lua error: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
        return;
    }
    if (lua_gettop(g_lua) > 0) {
        logf("lua: %s", luaL_tolstring(g_lua, -1, nullptr));
        lua_pop(g_lua, 2);
    }
}

}  // namespace bg3le
