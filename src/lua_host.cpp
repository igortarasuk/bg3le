#include "lua_host.h"

#include "log.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace bg3le {
namespace {
lua_State* g_lua = nullptr;
}

void lua_init() {
    if (g_lua != nullptr) return;

    g_lua = luaL_newstate();
    if (g_lua == nullptr) {
        logf("lua: luaL_newstate failed");
        return;
    }
    luaL_openlibs(g_lua);

    if (luaL_dostring(g_lua, "return _VERSION") != LUA_OK) {
        logf("lua: smoke test failed: %s", lua_tostring(g_lua, -1));
        lua_pop(g_lua, 1);
        return;
    }
    logf("lua: %s up", lua_tostring(g_lua, -1));
    lua_pop(g_lua, 1);
}

}  // namespace bg3le
