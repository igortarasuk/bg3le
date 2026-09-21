#include "lua_host.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <string>
#include <utility>

#include "debug_server.h"
#include "ecs_types.h"
#include "ecs_world.h"
#include "mem.h"
#include "log.h"

// Norbyte's Lua fork is compiled as C++, as bg3se compiles it, so these must
// not be wrapped in extern "C" or the symbols will not match.
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

namespace bg3le {
namespace {

lua_State* g_lua = nullptr;
const SymbolTable* g_symbols = nullptr;

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

    // With out-param counts recovered from Osiris, the input count is known
    // exactly and a wrong count is an error, as in bg3se. Without them, fall
    // back to letting the caller's argument count decide the split.
    if (fn->out_params >= 0) {
        const int expected = (int)fn->params.size() - fn->out_params;
        if (argc != expected) {
            return luaL_error(L,
                "Incorrect number of IN arguments for '%s'; expected %d, got %d",
                fn->name.c_str(), expected, argc);
        }
    } else if (static_cast<std::size_t>(argc) > fn->params.size()) {
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
    const osi::Status status = osi::invoke(*fn, inputs, &outputs);

    if (status == osi::Status::kUnavailable) {
        return luaL_error(L, "Osi.%s could not be invoked (Osiris not ready?)",
                          fn->name.c_str());
    }

    // Matches bg3se: a procedure yields nothing, a query with no outputs
    // yields the success flag, and a query with outputs yields one value per
    // output -- all nil when the engine answered false. Returning a bare nil
    // for both "answered false" and "could not call" conflated a normal
    // answer with an error.
    if (fn->kind() == osi::kCall) return 0;

    const int out_count = fn->out_params >= 0
                              ? fn->out_params
                              : static_cast<int>(fn->params.size() - inputs.size());
    if (out_count == 0) {
        lua_pushboolean(L, status == osi::Status::kHandled);
        return 1;
    }

    if (status == osi::Status::kHandled) {
        for (const osi::Value& v : outputs) push_value(L, v);
    } else {
        for (int i = 0; i < out_count; ++i) lua_pushnil(L);
    }
    return out_count;
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

// Raw memory access, so structure walks can be prototyped from the live
// console instead of rebuilding and reloading a save for every guess.
// Reads are fault-tolerant: a wrong address returns nil, it does not crash
// the game.
int l_module_base(lua_State* L) {
    const char* want = luaL_checkstring(L, 1);
    struct Ctx { const char* want; std::uintptr_t base; } ctx{want, 0};
    ::dl_iterate_phdr(
        [](struct dl_phdr_info* info, std::size_t, void* data) {
            auto* c = static_cast<Ctx*>(data);
            const char* name = info->dlpi_name;
            if (c->want[0] == '\0') {
                if (name == nullptr || name[0] == '\0') {
                    c->base = info->dlpi_addr;
                    return 1;
                }
                return 0;
            }
            if (name != nullptr && std::strstr(name, c->want) != nullptr) {
                c->base = info->dlpi_addr;
                return 1;
            }
            return 0;
        },
        &ctx);
    if (ctx.base == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(ctx.base));
    return 1;
}

int l_peek(lua_State* L) {
    const auto addr = static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));
    const int width = static_cast<int>(luaL_optinteger(L, 2, 8));
    std::uint64_t value = 0;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        return luaL_error(L, "Peek width must be 1, 2, 4 or 8");
    }
    if (!safe_read(reinterpret_cast<const void*>(addr), &value, width)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

int l_peek_string(lua_State* L) {
    const auto addr = static_cast<std::uintptr_t>(luaL_checkinteger(L, 1));
    char buf[512];
    if (!safe_cstr(reinterpret_cast<const void*>(addr), buf, sizeof(buf))) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, buf);
    return 1;
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

// The project root, derived from our own .so path: it lives in <root>/build/.
// Mod discovery uses it to find <root>/mods without hardcoding a path.
int l_extender_root(lua_State* L) {
    ::Dl_info info{};
    if (::dladdr(reinterpret_cast<const void*>(&l_extender_root), &info) == 0 ||
        info.dli_fname == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    std::string path(info.dli_fname);
    for (int up = 0; up < 2; ++up) {  // strip the filename, then build/
        const std::size_t slash = path.find_last_of('/');
        if (slash == std::string::npos) {
            lua_pushnil(L);
            return 1;
        }
        path.erase(slash);
    }
    lua_pushstring(L, path.c_str());
    return 1;
}

// Mod discovery needs to enumerate directories, which Lua cannot do without
// shelling out. Returns names only, with "." and ".." dropped.
int l_list_dir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    DIR* dir = ::opendir(path);
    if (dir == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, std::strerror(errno));
        return 2;
    }
    lua_newtable(L);
    int n = 0;
    while (dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) {
            continue;
        }
        lua_pushstring(L, e->d_name);
        lua_rawseti(L, -2, ++n);
    }
    ::closedir(dir);
    return 1;
}

// Norbyte's Lua fork expects the host to install these before anything can
// allocate, because its GC calls them unconditionally; luaL_newstate alone
// leaves them null and the first sweep jumps through a null pointer. bg3se
// installs its own in LuaStateWrapper. bg3le creates no LUA_TCPPOBJECT
// values, so only the allocator and the string-cache pair ever run.
void* cpp_alloc(lua_State*, std::size_t size) { return std::malloc(size); }
void cpp_free(lua_State*, void* block, std::size_t) { std::free(block); }
void cpp_finalize(lua_State*, void*) {}
void* cpp_canonicalize(lua_State*, void* val) { return val; }
CMetatable* cpp_get_metatable(lua_State*, void*, unsigned long long) { return nullptr; }
CMetatable* cpp_get_light_metatable(lua_State*, unsigned long long,
                                    unsigned long long) { return nullptr; }
void cache_string(lua_State*, TString*) {}
void release_string(lua_State*, TString*) {}

// Address of an engine symbol by mangled name. The counterpart to Peek: with
// both, a structure can be walked from the prompt without a rebuild.
int l_symbol_addr(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    if (g_symbols == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    void* addr = g_symbols->find(name);
    if (addr == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(addr)));
    return 1;
}

// The engine's ECS type index for a component, read live from the static the
// engine assigns at startup. This is the foundation Ext.Entity needs.
int l_component_index(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const auto idx = ecs::index_of(ecs::Context::Component, name);
    if (!idx.has_value()) {
        // Try the one-frame registry before giving up; a handful of components
        // are registered there instead.
        const auto one_frame = ecs::index_of(ecs::Context::OneFrameComponent, name);
        if (!one_frame.has_value()) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushinteger(L, *one_frame);
        lua_pushstring(L, "one-frame");
        return 2;
    }
    lua_pushinteger(L, *idx);
    return 1;
}

// The captured ECS storage pointer, for verifying the capture works before
// anything is built on it.
int l_ecs_storage(lua_State* L) {
    void* p = ecs::storage();
    if (p == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(p)));
    return 1;
}

int l_ecs_counts(lua_State* L) {
    lua_newtable(L);
    const std::pair<ecs::Context, const char*> contexts[] = {
        {ecs::Context::Component, "Component"},
        {ecs::Context::OneFrameComponent, "OneFrameComponent"},
        {ecs::Context::System, "System"},
        {ecs::Context::Replication, "Replication"},
        {ecs::Context::ImmutableData, "ImmutableData"},
        {ecs::Context::Unchecked, "Unchecked"},
    };
    for (const auto& [ctx, label] : contexts) {
        lua_pushinteger(L, (lua_Integer)ecs::count(ctx));
        lua_setfield(L, -2, label);
    }
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
    lua_setup_cppobjects(g_lua, &cpp_alloc, &cpp_free, &cpp_get_light_metatable,
                         &cpp_get_metatable, &cpp_finalize, &cpp_canonicalize);
    lua_setup_strcache(g_lua, &cache_string, &release_string);
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

    lua_newtable(g_lua);                       // Ext._Internal
    lua_pushcfunction(g_lua, l_module_base);
    lua_setfield(g_lua, -2, "ModuleBase");
    lua_pushcfunction(g_lua, l_peek);
    lua_setfield(g_lua, -2, "Peek");
    lua_pushcfunction(g_lua, l_peek_string);
    lua_setfield(g_lua, -2, "PeekString");
    lua_pushcfunction(g_lua, l_list_dir);
    lua_setfield(g_lua, -2, "ListDir");
    lua_pushcfunction(g_lua, l_extender_root);
    lua_setfield(g_lua, -2, "ExtenderRoot");
    lua_pushcfunction(g_lua, l_component_index);
    lua_setfield(g_lua, -2, "ComponentIndex");
    lua_pushcfunction(g_lua, l_ecs_counts);
    lua_setfield(g_lua, -2, "EcsCounts");
    lua_pushcfunction(g_lua, l_symbol_addr);
    lua_setfield(g_lua, -2, "SymbolAddr");
    lua_pushcfunction(g_lua, l_ecs_storage);
    lua_setfield(g_lua, -2, "EcsStorage");
    lua_setfield(g_lua, -2, "_Internal");

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

-- Timers are driven from the server tick, so callbacks run on the story
-- thread and may call Osiris.
local timers, next_handle = {}, 1

function Ext.Timer.WaitFor(ms, fn, repeat_ms)
  local handle = next_handle
  next_handle = handle + 1
  timers[handle] = {
    due = Ext.Timer.MonotonicTime() + ms, fn = fn, every = repeat_ms
  }
  return handle
end

function Ext.Timer.Cancel(handle) timers[handle] = nil end

function Ext._Internal.RunTimers()
  local now = Ext.Timer.MonotonicTime()
  for handle, t in pairs(timers) do
    if now >= t.due then
      if t.every then t.due = now + t.every else timers[handle] = nil end
      local ok, err = pcall(t.fn)
      if not ok then
        Ext.Log.PrintError("Timer callback failed: " .. tostring(err))
      end
    end
  end
end

-- ---- mod loading ----
--
-- Loose-file mods only: a root on the search path is a directory containing
-- Mods/<Name>/ScriptExtender/. Reading .pak archives is not implemented.
local loaded = {}

local function read_file(path)
  local f = io.open(path, "rb")
  if not f then return nil end
  local text = f:read("a")
  f:close()
  return text
end

local function mod_roots()
  local roots = {}
  local env = os.getenv("BG3LE_MOD_PATH")
  if env then
    for dir in string.gmatch(env, "[^:]+") do table.insert(roots, dir) end
  end
  local root = Ext._Internal.ExtenderRoot()
  if root then table.insert(roots, root .. "/mods") end
  return roots
end

-- Config.json only ever needs ModTable here, so match it directly rather
-- than pulling in a JSON parser.
local function mod_table_name(config)
  return string.match(config, '"ModTable"%s*:%s*"([^"]+)"')
end

local function load_mod(root, name)
  local dir = root .. "/Mods/" .. name .. "/ScriptExtender"
  local config = read_file(dir .. "/Config.json")
  if not config then return end

  local table_name = mod_table_name(config)
  if not table_name then
    Ext.Log.PrintWarning(string.format(
      "bg3le: %s has no ModTable in Config.json; skipping", name))
    return
  end
  if loaded[table_name] then return end

  local bootstrap = dir .. "/Lua/BootstrapServer.lua"
  local source = read_file(bootstrap)
  if not source then return end

  Mods[table_name] = Mods[table_name] or {}

  -- Ext.Require resolves against the mod currently being loaded, as it does
  -- in bg3se.
  local lua_dir = dir .. "/Lua"
  function Ext.Require(path)
    local text = read_file(lua_dir .. "/" .. path)
    if not text then
      error("bg3le: Ext.Require could not read " .. path, 0)
    end
    local chunk, err = load(text, "@" .. path)
    if not chunk then error(err, 0) end
    return chunk()
  end

  local chunk, err = load(source, "@" .. name .. "/BootstrapServer.lua")
  if not chunk then
    Ext.Log.PrintError(string.format("bg3le: %s failed to compile: %s", name, err))
    return
  end
  local ok, run_err = pcall(chunk)
  if not ok then
    Ext.Log.PrintError(string.format("bg3le: %s failed to load: %s", name, run_err))
    return
  end

  loaded[table_name] = true
  Ext.Log.Print(string.format("bg3le: loaded mod %s (Mods.%s)", name, table_name))
end

function Ext._Internal.LoadMods()
  for _, root in ipairs(mod_roots()) do
    local names = Ext._Internal.ListDir(root .. "/Mods")
    if names then
      table.sort(names)
      for _, name in ipairs(names) do load_mod(root, name) end
    end
  end
end
)LUA";
    if (luaL_dostring(g_lua, kPrelude) != LUA_OK) {
        logf("lua: prelude failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
    statusf("LUA VM initialised (%s)", LUA_RELEASE);
}

// Calls a niladic Ext._Internal function, if it is present. Errors are logged
// rather than propagated: this runs on the game's own threads.
void call_internal(const char* name) {
    if (g_lua == nullptr) return;
    lua_getglobal(g_lua, "Ext");
    if (!lua_istable(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    lua_getfield(g_lua, -1, "_Internal");
    lua_remove(g_lua, -2);
    if (!lua_istable(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    lua_getfield(g_lua, -1, name);
    lua_remove(g_lua, -2);
    if (!lua_isfunction(g_lua, -1)) {
        lua_pop(g_lua, 1);
        return;
    }
    if (lua_pcall(g_lua, 0, 0, 0) != LUA_OK) {
        logf("lua: Ext._Internal.%s failed: %s", name, lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
    }
}

void lua_set_symbols(const SymbolTable* symbols) { g_symbols = symbols; }

void lua_tick() { call_internal("RunTimers"); }

void lua_load_mods() { call_internal("LoadMods"); }

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

    statusf("Bound %d Osiris functions as Osi.* and globals (%d events skipped)",
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


// Norbyte's Lua fork calls this from luaG_errormsg whenever an error is raised
// while an error handler is installed, so a debugger can see errors that pcall
// would otherwise swallow. The host has to supply it or the fork does not link.
//
// Weak, because bg3se's Lua/LuaBinding.cpp defines it too: once
// vendor/bg3se is linked in, its strong definition takes precedence and this
// one falls away.
//
// It fires for every handled error, including the deliberate ones in our timer
// and mod-loading paths, so this goes to the log rather than the console.
__attribute__((weak))
void nse_lua_report_handled_error(lua_State* L) {
    const char* err = lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1)
                                                     : "(not a string)";
    bg3le::logf("lua: handled error: %s", err);
}
