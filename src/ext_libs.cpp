// The parts of Ext.Utils, Ext.IO, Ext.Timer and Ext.Debug that need more
// than Lua: clocks, the game's version, a GUID generator, and file access
// under the profile and data roots.
//
// Everything here mirrors the behaviour of its counterpart in bg3se's
// Lua/Libs, with the platform-specific halves replaced. Upstream's
// MicrosecTime is QueryPerformanceCounter against a counter taken at
// startup; this keeps the same shape with a steady_clock baseline.

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "lauxlib.h"
#include "lua.h"

#include "log.h"

namespace bg3le {

namespace {

std::chrono::steady_clock::time_point const kStart =
    std::chrono::steady_clock::now();

// Where SaveFile writes and LoadFile reads by default, matching upstream's
// PathRootType::UserProfile.
std::string profile_root() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home) + "/.local/share/Larian Studios/Baldur's Gate 3";
}

// PathRootType::Data, the game's own install.
std::string data_root() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';

    std::string path(exe);
    const std::size_t bin = path.rfind("/bin/");
    if (bin == std::string::npos) return {};
    return path.substr(0, bin) + "/Data";
}

// Refuses to leave the root it was given: a mod naming "../../.ssh/id_rsa"
// should not reach it, and upstream's script::LoadExternalFile checks the
// same way.
bool resolve_under(std::string const& root, char const* relative,
                   std::string* out) {
    if (root.empty() || relative == nullptr || relative[0] == '\0') {
        return false;
    }
    if (relative[0] == '/') return false;
    if (std::strstr(relative, "..") != nullptr) return false;

    *out = root + "/" + relative;
    return true;
}

bool make_parents(std::string const& path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos) return true;

    std::string dir = path.substr(0, slash);
    for (std::size_t i = 1; i <= dir.size(); ++i) {
        if (i != dir.size() && dir[i] != '/') continue;
        std::string const part = dir.substr(0, i);
        if (mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
    return true;
}

}  // namespace

// ---- clocks ---------------------------------------------------------------

extern "C" int bg3le_ext_monotonic_time(lua_State* L) {
    using namespace std::chrono;
    lua_pushinteger(L, (lua_Integer)duration_cast<milliseconds>(
                           steady_clock::now().time_since_epoch()).count());
    return 1;
}

extern "C" int bg3le_ext_microsec_time(lua_State* L) {
    using namespace std::chrono;
    const auto since = steady_clock::now() - kStart;
    lua_pushnumber(L, (lua_Number)duration_cast<nanoseconds>(since).count()
                          / 1000.0);
    return 1;
}

extern "C" int bg3le_ext_clock_epoch(lua_State* L) {
    using namespace std::chrono;
    lua_pushinteger(L, (lua_Integer)duration_cast<seconds>(
                           system_clock::now().time_since_epoch()).count());
    return 1;
}

extern "C" int bg3le_ext_clock_time(lua_State* L) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
    localtime_r(&now, &parts);

    char text[64];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &parts);
    lua_pushstring(L, text);
    return 1;
}

// ---- identity -------------------------------------------------------------

extern "C" int bg3le_ext_generate_guid(lua_State* L) {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uint8_t bytes[16];
    for (int i = 0; i < 16; i += 8) {
        const std::uint64_t word = rng();
        std::memcpy(bytes + i, &word, 8);
    }
    // Version 4, variant 1, as Guid::Generate does.
    bytes[6] = (std::uint8_t)((bytes[6] & 0x0f) | 0x40);
    bytes[8] = (std::uint8_t)((bytes[8] & 0x3f) | 0x80);

    char text[37];
    std::snprintf(text, sizeof(text),
                  "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                  "%02x%02x%02x%02x%02x%02x",
                  bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                  bytes[6], bytes[7], bytes[8], bytes[9], bytes[10],
                  bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    lua_pushstring(L, text);
    return 1;
}

// The game's version, in the form upstream reports.
//
// There is no PE version resource to read here, so it comes from the
// version string the binary carries, "4.1.1.7398727". Larian's last
// component packs the rest: 73 * 100000 + 98 * 1000 + 727 = 7398727, which
// is v4.73.98.727 -- exactly what the Windows extender reported for this
// same build in reference/utils-shape.txt.
extern "C" int bg3le_ext_game_version(lua_State* L) {
    static std::string cached;
    if (!cached.empty()) {
        lua_pushstring(L, cached.c_str());
        return 1;
    }

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return 0;
    exe[n] = '\0';

    std::FILE* f = std::fopen(exe, "rb");
    if (f == nullptr) return 0;

    // A bounded scan for the version literal rather than a full read of a
    // 300 MB binary.
    std::string window;
    std::vector<char> block(1u << 20);
    unsigned major = 0;
    unsigned a = 0;
    unsigned b = 0;
    unsigned packed = 0;
    bool found = false;
    while (!found) {
        const std::size_t got = std::fread(block.data(), 1, block.size(), f);
        if (got == 0) break;

        window.append(block.data(), got);
        for (std::size_t i = 0; i + 8 < window.size() && !found; ++i) {
            if (window[i] < '0' || window[i] > '9') continue;
            if (std::sscanf(window.c_str() + i, "%u.%u.%u.%u", &major, &a, &b,
                            &packed) != 4) {
                continue;
            }
            if (major == 4 && packed > 100000) found = true;
        }
        if (window.size() > (1u << 20)) {
            window.erase(0, window.size() - 64);
        }
    }
    std::fclose(f);
    if (!found) return 0;

    char text[64];
    std::snprintf(text, sizeof(text), "v%u.%u.%u.%u", major, packed / 100000,
                  (packed / 1000) % 100, packed % 1000);
    cached = text;
    lua_pushstring(L, cached.c_str());
    return 1;
}

extern "C" int bg3le_ext_command_line(lua_State* L) {
    std::FILE* f = std::fopen("/proc/self/cmdline", "rb");
    if (f == nullptr) return 0;

    std::string all;
    char block[4096];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
        all.append(block, got);
    }
    std::fclose(f);

    lua_newtable(L);
    int n = 0;
    std::size_t start = 0;
    while (start < all.size()) {
        const std::size_t end = all.find('\0', start);
        const std::string arg = all.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!arg.empty()) {
            lua_pushstring(L, arg.c_str());
            lua_rawseti(L, -2, ++n);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return 1;
}

// ---- files ----------------------------------------------------------------

// Ext.IO.LoadFile(path[, context]) where context is "user" or "data".
extern "C" int bg3le_ext_load_file(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    char const* context = lua_isnoneornil(L, 2) ? "user"
                                                : luaL_checkstring(L, 2);

    std::string root;
    if (std::strcmp(context, "user") == 0) {
        root = profile_root();
    } else if (std::strcmp(context, "data") == 0) {
        root = data_root();
    } else {
        return luaL_error(L, "Unknown file loading context: %s", context);
    }

    std::string path;
    if (!resolve_under(root, relative, &path)) return 0;

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return 0;

    std::string contents;
    char block[65536];
    std::size_t got = 0;
    while ((got = std::fread(block, 1, sizeof(block), f)) > 0) {
        contents.append(block, got);
    }
    std::fclose(f);

    lua_pushlstring(L, contents.data(), contents.size());
    return 1;
}

// Ext.IO.SaveFile(path, contents), and AppendFile through the same path.
extern "C" int bg3le_ext_save_file(lua_State* L) {
    char const* relative = luaL_checkstring(L, 1);
    std::size_t length = 0;
    char const* contents = luaL_checklstring(L, 2, &length);
    const bool append = lua_toboolean(L, 3) != 0;

    std::string path;
    if (!resolve_under(profile_root(), relative, &path)
        || !make_parents(path)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    std::FILE* f = std::fopen(path.c_str(), append ? "ab" : "wb");
    if (f == nullptr) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const bool ok = length == 0 || std::fwrite(contents, 1, length, f) == length;
    std::fclose(f);

    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ---- misc -----------------------------------------------------------------

extern "C" int bg3le_ext_memory_usage(lua_State* L) {
    // Upstream reports the extender's own allocation total. bg3le does not
    // track one, so this is Lua's heap, which is what a mod asking about
    // memory usage from Lua is generally after.
    const int kb = lua_gc(L, LUA_GCCOUNT, 0);
    lua_pushinteger(L, (lua_Integer)kb * 1024);
    return 1;
}

extern "C" int bg3le_ext_show_error(lua_State* L) {
    char const* message = luaL_checkstring(L, 1);
    logf("Ext.Utils.ShowError: %s", message);
    std::fprintf(stderr, "bg3le: %s\n", message);
    return 0;
}

}  // namespace bg3le
