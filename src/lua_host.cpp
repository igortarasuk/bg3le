#include "lua_host.h"

#include <string>

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

}  // namespace

void lua_init() {
    if (g_lua != nullptr) return;

    g_lua = luaL_newstate();
    if (g_lua == nullptr) {
        logf("lua: luaL_newstate failed");
        return;
    }
    luaL_openlibs(g_lua);
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
    logf("lua: bound %d Osi functions (%d events skipped)", bound, events);
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
