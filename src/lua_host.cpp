#include "lua_host.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <dlfcn.h>
#include <link.h>
#include <optional>
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

// The captured EntityStorageContainer, for verifying the capture works
// before anything is built on it.
int l_ecs_storage(lua_State* L) {
    void* p = ecs::container();
    if (p == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<std::uintptr_t>(p)));
    return 1;
}

// Reads the captured container's Storages array, which bg3se says is its
// first member: Array<EntityStorageData*> = {buf, size}. One call is enough to
// tell whether the capture is a real container or a coincidence.
int l_ecs_dump(lua_State* L) {
    void* c = ecs::container();
    if (c == nullptr) {
        lua_pushnil(L);
        lua_pushstring(L, "no entity lookup has happened yet");
        return 2;
    }

    std::uintptr_t buf = 0;
    std::uint32_t size = 0;
    const auto base = reinterpret_cast<const char*>(c);
    if (!safe_read(base, &buf, sizeof(buf)) ||
        !safe_read(base + sizeof(buf), &size, sizeof(size))) {
        lua_pushnil(L);
        lua_pushstring(L, "container is not readable");
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(c));
    lua_setfield(L, -2, "Container");
    lua_pushinteger(L, (lua_Integer)buf);
    lua_setfield(L, -2, "StoragesBuf");
    lua_pushinteger(L, (lua_Integer)size);
    lua_setfield(L, -2, "StoragesCount");

    // A handful of storage pointers, to show the array holds pointers rather
    // than noise.
    lua_newtable(L);
    const std::uint32_t show = size < 6 ? size : 6;
    for (std::uint32_t i = 0; i < show; ++i) {
        std::uintptr_t entry = 0;
        if (!safe_read(reinterpret_cast<const char*>(buf) + i * sizeof(entry),
                       &entry, sizeof(entry))) {
            break;
        }
        lua_pushinteger(L, (lua_Integer)entry);
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "Storages");
    return 1;
}

// Implemented in src/vendor/entity_bridge.cpp against bg3se's ECS layout.
extern "C" bool bg3le_entity_health(void* container, std::uint64_t handle,
                                    std::uint16_t componentIndex,
                                    std::int32_t* hp, std::int32_t* maxHp);

extern "C" void bg3le_entity_probe(void* container, std::uint64_t handle,
                                   std::uint16_t componentIndex,
                                   std::int32_t* storageIndex, void** storage,
                                   void** component);
extern "C" bool bg3le_mark_component_changed(void* container, std::uint64_t handle,
                                             std::uint16_t componentIndex);
extern "C" bool bg3le_set_health(void* container, std::uint64_t handle,
                                 std::uint16_t componentIndex, std::int32_t hp,
                                 bool setMax);
extern "C" void bg3le_world_probe(void* container, void** world,
                                  void** replication, std::int32_t* poolCount,
                                  bool* storageMatches, bool* queriesMatch);
extern "C" std::int32_t bg3le_replicate_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t replicationTypeIndex,
                                                  std::uint32_t qword,
                                                  std::uint64_t flags);

extern "C" bool bg3le_container_is_server(void* container);
extern "C" bool bg3le_game_allocator_ready();

// The component field tables, from src/vendor/component_meta.cpp.
extern "C" void const* bg3le_meta_component(const char* engineName);
extern "C" std::size_t bg3le_meta_component_size(void const* handle);
extern "C" std::size_t bg3le_meta_component_stride(void const* handle);
extern "C" bool bg3le_meta_component_is_proxy(void const* handle);
extern "C" bool bg3le_meta_field(void const* handle, const char* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount);
extern "C" std::size_t bg3le_meta_fields(void const* handle, const char** names,
                                         std::uint8_t* kinds,
                                         std::size_t capacity);
extern "C" std::size_t bg3le_meta_fields_at(void const* handle,
                                            const char* path,
                                            const char** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity);
extern "C" bool bg3le_meta_resolve(void const* handle, const char* path,
                                   void* component, void** address,
                                   std::uint8_t* kind, std::uint16_t* size,
                                   bool* readOnly);
extern "C" int bg3le_meta_array_length(void const* handle, const char* path,
                                       void* component, std::size_t* count,
                                       std::uint16_t* elemSize,
                                       std::uint8_t* elemKind);
extern "C" bool bg3le_meta_map_key(void const* handle, const char* path,
                                   void* component, std::size_t index,
                                   void** address, std::uint8_t* kind,
                                   std::uint16_t* size);
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);
extern "C" std::size_t bg3le_meta_class_count();
extern "C" std::size_t bg3le_meta_component_count();
extern "C" const char* bg3le_meta_engine_class(void const* handle);
extern "C" const char* bg3le_meta_short_name(void const* handle);
extern "C" void* bg3le_entity_component(void* container, std::uint64_t handle,
                                        std::uint16_t componentIndex,
                                        std::size_t componentSize);

// The container belonging to the server world.
//
// Both worlds come through the capture thunk and the order is not ours to
// choose, so the slots are sorted out here by asking which one has replication
// buffers. Server-side script runs against server state: reading the client's
// copy of a component gives a value that looks right but is a replica, and
// writing it is overwritten on the next update.
//
// Falls back to whatever was captured first if neither slot identifies as the
// server yet, so nothing that used to work stops working before the second
// container has been seen.
void* server_container() {
    if (bg3le_container_is_server(ecs::container())) return ecs::container();
    if (bg3le_container_is_server(ecs::container_alt())) return ecs::container_alt();
    return ecs::container();
}

// Accepts either name a component goes by and yields the engine's.
//
// bg3se describes 1,071 components and the symbol table has rather more, so a
// name it has no metadata for is passed through unchanged. That keeps the raw
// engine names working for the components bg3se has not mapped, which is the
// only way to reach them at all.
const char* engine_name_of(const char* name) {
    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) return name;
    const char* engine = bg3le_meta_engine_class(meta);
    return engine != nullptr ? engine : name;
}

// Resolves a component name to its engine index, trying the one-frame registry
// as a fallback.
std::optional<std::int32_t> component_index(const char* name) {
    if (auto i = ecs::index_of(ecs::Context::Component, name)) return i;
    return ecs::index_of(ecs::Context::OneFrameComponent, name);
}

// Generic component access, driven by bg3se's field tables rather than by a
// hand-written accessor per component.
//
// The type decode stays here rather than in the bridge so that Lua and bg3se
// remain in separate translation units: the bridge hands back a raw component
// pointer, and the offset and kind come from the metadata.
//
// Mirrors bg3le::FieldKind in src/component_meta_abi.h.
enum class FieldKind : std::uint8_t {
    Unsupported = 0, Bool, Float, Double, Int8, Uint8, Int16, Uint16,
    Int32, Uint32, Int64, Uint64, Guid, Entity, ScalarArray, Struct,
    DynArray, Map, Inherit,
};

const char* field_kind_name(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: return "boolean";
        case FieldKind::Float: return "float";
        case FieldKind::Double: return "double";
        case FieldKind::Int8: return "int8";
        case FieldKind::Uint8: return "uint8";
        case FieldKind::Int16: return "int16";
        case FieldKind::Uint16: return "uint16";
        case FieldKind::Int32: return "int32";
        case FieldKind::Uint32: return "uint32";
        case FieldKind::Int64: return "int64";
        case FieldKind::Uint64: return "uint64";
        case FieldKind::Guid: return "guid";
        case FieldKind::Entity: return "entity";
        case FieldKind::ScalarArray: return "array";
        case FieldKind::Struct: return "struct";
        case FieldKind::DynArray: return "array";
        case FieldKind::Map: return "map";
        default: return "unsupported";
    }
}

// The stride of a scalar kind, used to walk a fixed-extent array. Zero for
// anything that is not a scalar, which stops an array of them being walked.
std::size_t field_kind_size(FieldKind kind) {
    switch (kind) {
        case FieldKind::Bool: case FieldKind::Int8: case FieldKind::Uint8:
            return 1;
        case FieldKind::Int16: case FieldKind::Uint16:
            return 2;
        case FieldKind::Float: case FieldKind::Int32: case FieldKind::Uint32:
            return 4;
        case FieldKind::Double: case FieldKind::Int64: case FieldKind::Uint64:
        case FieldKind::Entity:
            return 8;
        case FieldKind::Guid:
            return 16;
        default:
            return 0;
    }
}

// Resolves an entity's component to a live pointer, using the metadata's size
// as the page stride and the symbol table's index as the type.
// name may be either the engine name or bg3se's short name; the index is
// always looked up under the engine name, which the metadata supplies.
void* component_pointer(std::uint64_t handle, const char* name,
                        void const** meta) {
    *meta = bg3le_meta_component(name);
    if (*meta == nullptr) return nullptr;

    const char* engineName = bg3le_meta_engine_class(*meta);
    if (engineName == nullptr) return nullptr;

    const auto index = component_index(engineName);
    if (!index.has_value()) return nullptr;

    // The stride, not the struct size. For a proxy component the page holds a
    // pointer and the struct lives wherever it points, so passing the struct
    // size would stride the page wrongly and then read the pointer's own bytes
    // as the first fields. 46 of the components a live save carries are
    // proxies, so this is not an edge case.
    void* slot = bg3le_entity_component(server_container(), handle,
                                        static_cast<std::uint16_t>(*index),
                                        bg3le_meta_component_stride(*meta));
    if (slot == nullptr) return nullptr;

    if (bg3le_meta_component_is_proxy(*meta)) {
        void* target = nullptr;
        if (!safe_read(slot, &target, sizeof(target))) return nullptr;
        return target;
    }
    return slot;
}

// Pushes a field, read through safe_read so a stale handle yields nil rather
// than a fault.
bool push_field(lua_State* L, const void* address, FieldKind kind,
                FieldKind elemKind = FieldKind::Unsupported,
                std::uint16_t elemCount = 0) {
    std::uint64_t raw = 0;
    switch (kind) {
        case FieldKind::ScalarArray: {
            // One-based, as Lua tables are. The stride comes from the element
            // kind, and a partial read fails the whole field rather than
            // returning a short table.
            const std::size_t stride = field_kind_size(elemKind);
            if (stride == 0) return false;
            lua_createtable(L, elemCount, 0);
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                if (!push_field(L, (const char*)address + i * stride,
                                elemKind)) {
                    lua_pop(L, 1);
                    return false;
                }
                lua_rawseti(L, -2, i + 1);
            }
            return true;
        }
        case FieldKind::Bool:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushboolean(L, (int)(raw & 0xff));
            return true;
        case FieldKind::Int8:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushinteger(L, (std::int8_t)raw);
            return true;
        case FieldKind::Uint8:
            if (!safe_read(address, &raw, 1)) return false;
            lua_pushinteger(L, (std::uint8_t)raw);
            return true;
        case FieldKind::Int16:
            if (!safe_read(address, &raw, 2)) return false;
            lua_pushinteger(L, (std::int16_t)raw);
            return true;
        case FieldKind::Uint16:
            if (!safe_read(address, &raw, 2)) return false;
            lua_pushinteger(L, (std::uint16_t)raw);
            return true;
        case FieldKind::Int32:
            if (!safe_read(address, &raw, 4)) return false;
            lua_pushinteger(L, (std::int32_t)raw);
            return true;
        case FieldKind::Uint32:
            if (!safe_read(address, &raw, 4)) return false;
            lua_pushinteger(L, (std::uint32_t)raw);
            return true;
        case FieldKind::Int64:
        case FieldKind::Entity:
            if (!safe_read(address, &raw, 8)) return false;
            lua_pushinteger(L, (lua_Integer)(std::int64_t)raw);
            return true;
        case FieldKind::Uint64:
            if (!safe_read(address, &raw, 8)) return false;
            lua_pushinteger(L, (lua_Integer)raw);
            return true;
        case FieldKind::Float: {
            float f = 0;
            if (!safe_read(address, &f, 4)) return false;
            lua_pushnumber(L, f);
            return true;
        }
        case FieldKind::Double: {
            double d = 0;
            if (!safe_read(address, &d, 8)) return false;
            lua_pushnumber(L, d);
            return true;
        }
        case FieldKind::Guid: {
            // Formatted by bg3se rather than here. The byte order is not the
            // obvious one -- see bg3le_meta_format_guid -- and open-coding it
            // produced a UUID that looked right and was not.
            std::uint8_t b[16];
            if (!safe_read(address, b, sizeof(b))) return false;
            char text[40];
            if (!bg3le_meta_format_guid(b, text, sizeof(text))) return false;
            lua_pushstring(L, text);
            return true;
        }
        default:
            return false;
    }
}

// Writes a field. Narrower than the read side on purpose: only the numeric
// and boolean kinds, because writing a GUID or an entity handle by value is
// not something a mod should be doing by accident.
bool write_field(lua_State* L, int index, void* address, FieldKind kind,
                 FieldKind elemKind = FieldKind::Unsupported,
                 std::uint16_t elemCount = 0) {
    switch (kind) {
        case FieldKind::ScalarArray: {
            const std::size_t stride = field_kind_size(elemKind);
            if (stride == 0 || !lua_istable(L, index)) return false;
            // Written element-wise so a short table leaves the rest alone
            // rather than zeroing it.
            for (std::uint16_t i = 0; i < elemCount; ++i) {
                lua_rawgeti(L, index, i + 1);
                if (!lua_isnil(L, -1)) {
                    if (!write_field(L, lua_gettop(L),
                                     (char*)address + i * stride, elemKind)) {
                        lua_pop(L, 1);
                        return false;
                    }
                }
                lua_pop(L, 1);
            }
            return true;
        }
        case FieldKind::Bool: {
            const std::uint8_t v = lua_toboolean(L, index) != 0 ? 1 : 0;
            std::memcpy(address, &v, 1);
            return true;
        }
        case FieldKind::Int8: case FieldKind::Uint8: {
            const auto v = (std::uint8_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 1);
            return true;
        }
        case FieldKind::Int16: case FieldKind::Uint16: {
            const auto v = (std::uint16_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 2);
            return true;
        }
        case FieldKind::Int32: case FieldKind::Uint32: {
            const auto v = (std::uint32_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 4);
            return true;
        }
        case FieldKind::Int64: case FieldKind::Uint64: {
            const auto v = (std::uint64_t)luaL_checkinteger(L, index);
            std::memcpy(address, &v, 8);
            return true;
        }
        case FieldKind::Float: {
            const auto v = (float)luaL_checknumber(L, index);
            std::memcpy(address, &v, 4);
            return true;
        }
        case FieldKind::Double: {
            const double v = luaL_checknumber(L, index);
            std::memcpy(address, &v, 8);
            return true;
        }
        default:
            return false;
    }
}

// Ext._Internal.GetField(handle, component, path)
//
// path may name a field, a field of a nested struct, or an element of an
// array: "Hp", "Transform.Translate", "Events[0].Amount". Resolving it is the
// C side's job, because an array element does not live at a fixed offset from
// the component -- its address is behind the container's own buffer pointer.
int l_get_field(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for %s", name);
        return 2;
    }
    if (component == nullptr) {
        lua_pushnil(L);
        return 1;  // the entity simply does not have this component
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    // Element kind and count only matter for a fixed-extent array, which
    // push_field turns into a table; the dynamic ones are proxied in Lua.
    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    if (!push_field(L, address, (FieldKind)kind, (FieldKind)elemKind,
                    elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is of an unsupported kind (%s)", name, path,
                        field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

// Ext._Internal.SetField(handle, component, path, value)
int l_set_field(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }
    if (readOnly) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "%s.%s is read-only: it is a hash set, and writing a key in place "
            "would leave the table's hashes stale", name, path);
        return 2;
    }

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    if (!write_field(L, 4, address, (FieldKind)kind, (FieldKind)elemKind,
                     elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not writable (%s)", name, path,
                        field_kind_name((FieldKind)kind));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// Ext._Internal.FieldInfo(component, path) -> kind, elemKind, elemCount
//
// Type information only, so it needs no entity. A dynamic array's length is
// not type information -- see ArrayInfo.
int l_field_info(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_component(name);
    if (meta == nullptr) return 0;

    std::uint32_t offset = 0;
    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(meta, path, &offset, &size, &kind, &elemKind,
                          &elemCount)) {
        return 0;
    }

    lua_pushstring(L, field_kind_name((FieldKind)kind));
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    lua_pushinteger(L, elemCount);
    return 3;
}

// Ext._Internal.ArrayInfo(handle, component, path) -> count, elementKind
//
// Needs the entity, because a dynamic array's length lives in the container
// rather than in the metadata. An element whose type is a struct bg3se
// describes is reported as "struct", so the caller knows to descend by path
// rather than to expect a value.
int l_array_info(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    const int status = bg3le_meta_array_length(meta, path, component, &count,
                                               &elemSize, &elemKind);
    if (status != 0) {
        static const char* const reasons[] = {
            "",
            "bad arguments",
            "the path does not resolve",
            "it is not a container",
            "it is a container with no length accessor",
        };
        lua_pushnil(L);
        lua_pushfstring(L, "cannot size %s.%s: %s", name, path,
                        (status >= 1 && status <= 4) ? reasons[status]
                                                     : "unknown error");
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)count);
    // An unsupported element kind on an array that resolved means the elements
    // are structs; whether they can be descended into was already decided by
    // the kind the field itself reports.
    lua_pushstring(L, (FieldKind)elemKind == FieldKind::Unsupported
                          ? "struct"
                          : field_kind_name((FieldKind)elemKind));
    return 2;
}

// Ext._Internal.MapKey(handle, component, path, index) -> key
//
// index is zero-based, matching the slot the value at the same index occupies,
// so walking the slots pairs keys with values.
int l_map_key(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);
    const auto index = (std::size_t)luaL_checkinteger(L, 4);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    if (!bg3le_meta_map_key(meta, path, component, index, &address, &kind,
                            &size)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s has no key at slot %d", name, path,
                        (int)index);
        return 2;
    }

    if (!push_field(L, address, (FieldKind)kind)) {
        lua_pushnil(L);
        lua_pushfstring(L,
            "the keys of %s.%s are of an unsupported kind (%s); the values are "
            "still reachable by slot", name, path,
            field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

extern "C" int bg3le_component_engine_size(void* container,
                                           std::uint16_t componentIndex);
extern "C" void const* bg3le_meta_class_at(std::size_t index);

// Ext._Internal.SizeAudit() -> { Checked, Matched, Mismatches = {...} }
//
// Compares every component's declared struct size against the size the engine
// recorded for it. A mismatch means every read of that component is
// misaligned, because the size is the stride GetComponent multiplies the
// entity's slot by -- so the entity at index 0 reads correctly and the rest do
// not. That is worth knowing across all of them at once rather than one
// surprising result at a time.
//
// Components no live entity carries are skipped: the engine only records a
// size where a storage holds one.
int l_size_audit(lua_State* L) {
    lua_newtable(L);
    lua_newtable(L);  // the mismatch list

    std::size_t checked = 0;
    std::size_t matched = 0;
    std::size_t mismatches = 0;

    const std::size_t classes = bg3le_meta_class_count();
    for (std::size_t i = 0; i < classes; ++i) {
        void const* cls = bg3le_meta_class_at(i);
        if (cls == nullptr) continue;
        const char* engineName = bg3le_meta_engine_class(cls);
        if (engineName == nullptr) continue;

        const auto index = component_index(engineName);
        if (!index.has_value()) continue;

        const int engineSize = bg3le_component_engine_size(
            server_container(), static_cast<std::uint16_t>(*index));
        if (engineSize < 0) continue;  // no live entity has it

        ++checked;
        const auto declared = (int)bg3le_meta_component_stride(cls);
        if (declared == engineSize) {
            ++matched;
            continue;
        }

        ++mismatches;
        lua_newtable(L);
        lua_pushstring(L, engineName);
        lua_setfield(L, -2, "Component");
        lua_pushinteger(L, declared);
        lua_setfield(L, -2, "Declared");
        lua_pushinteger(L, engineSize);
        lua_setfield(L, -2, "Engine");
        lua_rawseti(L, -2, (int)mismatches);
    }

    lua_setfield(L, -2, "Mismatches");
    lua_pushinteger(L, (lua_Integer)checked);
    lua_setfield(L, -2, "Checked");
    lua_pushinteger(L, (lua_Integer)matched);
    lua_setfield(L, -2, "Matched");
    return 1;
}

// Ext._Internal.FieldAddress(handle, component, path) -> address, size
//
// For probing a field whose layout is in doubt. A container's length and
// buffer are read through the engine's own accessors, which is right only if
// bg3se's idea of the container's shape matches the engine's -- and a set the
// engine populated is the only thing that can settle that. Pair this with
// Peek, or use FieldBytes.
int l_field_address(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                            &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)address);
    lua_pushinteger(L, size);
    return 2;
}

// Ext._Internal.FieldBytes(handle, component, path [, count])
//
// The field's raw bytes as hex, grouped in eights, so a struct's shape can be
// read off directly. Defaults to the field's own size.
int l_field_bytes(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = luaL_checkstring(L, 2);
    const char* path = luaL_checkstring(L, 3);

    void const* meta = nullptr;
    void* component = component_pointer(handle, name, &meta);
    if (meta == nullptr || component == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not available on this entity", name);
        return 2;
    }

    // An empty path means the component itself, which is how to see every
    // field's bytes at once -- the only way to check a container's shape
    // against what bg3se believes it to be.
    void* address = component;
    std::uint8_t kind = 0;
    std::uint16_t size = (std::uint16_t)bg3le_meta_component_size(meta);
    bool readOnly = false;
    if (path[0] != '\0'
        && !bg3le_meta_resolve(meta, path, component, &address, &kind, &size,
                               &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", name, path);
        return 2;
    }

    const auto want = (std::size_t)luaL_optinteger(L, 4, size);
    const std::size_t count = want > 256 ? 256 : want;

    std::string out;
    for (std::size_t i = 0; i < count; i += 8) {
        std::uint64_t word = 0;
        const std::size_t n = (count - i) < 8 ? (count - i) : 8;
        char line[64];
        if (!safe_read((const char*)address + i, &word, n)) {
            std::snprintf(line, sizeof(line), "+%02zx unreadable\n", i);
        } else {
            std::snprintf(line, sizeof(line), "+%02zx %016llx\n", i,
                          (unsigned long long)word);
        }
        out += line;
    }

    lua_pushstring(L, out.c_str());
    return 1;
}

// Ext._Internal.ComponentFields(name [, path]) ->{ field = kind, ... }, size
//
// path names a nested struct, so an inner struct lists the same way a
// component does.
int l_component_fields(lua_State* L) {
    const char* engineName = luaL_checkstring(L, 1);
    const char* path = luaL_optstring(L, 2, nullptr);
    void const* meta = bg3le_meta_component(engineName);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for %s", engineName);
        return 2;
    }

    constexpr std::size_t kMax = 512;
    const char* names[kMax];
    std::uint8_t kinds[kMax];
    const std::size_t n = bg3le_meta_fields_at(meta, path, names, kinds, kMax);
    if (n == 0 && path != nullptr && path[0] != '\0') {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s cannot be traversed", engineName, path);
        return 2;
    }

    lua_newtable(L);
    for (std::size_t i = 0; i < n; ++i) {
        lua_pushstring(L, field_kind_name((FieldKind)kinds[i]));
        lua_setfield(L, -2, names[i]);
    }
    lua_pushinteger(L, (lua_Integer)bg3le_meta_component_size(meta));
    return 2;
}

// Ext.Entity primitives. The Lua-visible object model is assembled in the
// prelude on top of these, so field access and assignment stay in one place.
int l_entity_get_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const auto index = component_index("eoc::HealthComponent");
    if (!index.has_value()) return 0;

    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    if (!bg3le_entity_health(server_container(), handle,
                             static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
        return 0;
    }
    lua_pushinteger(L, hp);
    lua_pushinteger(L, maxHp);
    return 2;
}

int l_entity_set_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const auto hp = static_cast<std::int32_t>(luaL_checkinteger(L, 2));
    const bool setMax = lua_toboolean(L, 3) != 0;
    const auto index = component_index("eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, bg3le_set_health(server_container(), handle,
                                        static_cast<std::uint16_t>(*index), hp,
                                        setMax));
    return 1;
}

int l_entity_mark_changed(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));
    const auto index = component_index(name);
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "%s is not a registered component", name);
        return 2;
    }
    lua_pushboolean(L, bg3le_mark_component_changed(
        server_container(), handle, static_cast<std::uint16_t>(*index)));
    return 1;
}

// Marking a component changed only tells the server. Replication is a second,
// separate registry: a component has a ReplicatedTypeContext index alongside
// its ComponentTypeIdContext one, and setting that entity's flags in the
// matching pool is what sends the value to the client.
int l_entity_replicate(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));

    const auto index = ecs::index_of(ecs::Context::Replication, name);
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a replicated component", name);
        return 2;
    }

    // Whole-component replication: qword 0, every flag set.
    const auto status = bg3le_replicate_component(
        server_container(), handle, static_cast<std::uint16_t>(*index), 0,
        ~static_cast<std::uint64_t>(0));
    if (status == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }

    static const char* const messages[] = {
        "",
        "the entity storage container has not been captured yet",
        "could not recover the entity world from the container",
        "this world has no replication buffers; only the server replicates",
        "the replication type index is outside the replication pool array",
        "could not add the entity to the replication pool",
        "the engine allocator is not installed, and this would have allocated",
    };
    static_assert(sizeof(messages) / sizeof(messages[0]) == 7,
                  "keep in step with bg3le_replicate_component status codes");
    lua_pushnil(L);
    lua_pushfstring(L, "could not replicate %s: %s", name,
                    (status >= 1 && status <= 5) ? messages[status] : "unknown error");
    return 2;
}

// Diagnostic for the world recovery: reports the derived pointers and whether
// the two independent cross-checks hold.
// Pushes one captured slot as a table. Both are reported, because which of
// them is the server is the whole question.
void push_world_probe(lua_State* L, void* container) {
    void* world = nullptr;
    void* replication = nullptr;
    std::int32_t poolCount = -1;
    bool storageMatches = false;
    bool queriesMatch = false;
    bg3le_world_probe(container, &world, &replication, &poolCount,
                      &storageMatches, &queriesMatch);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)container);
    lua_setfield(L, -2, "Container");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)world);
    lua_setfield(L, -2, "World");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)replication);
    lua_setfield(L, -2, "Replication");
    lua_pushinteger(L, poolCount);
    lua_setfield(L, -2, "ReplicationPools");
    lua_pushboolean(L, storageMatches);
    lua_setfield(L, -2, "StorageMatches");
    lua_pushboolean(L, queriesMatch);
    lua_setfield(L, -2, "QueriesMatch");
    lua_pushboolean(L, bg3le_container_is_server(container));
    lua_setfield(L, -2, "IsServer");
}

int l_world_probe(lua_State* L) {
    lua_newtable(L);
    push_world_probe(L, ecs::container());
    lua_setfield(L, -2, "Slot1");
    push_world_probe(L, ecs::container_alt());
    lua_setfield(L, -2, "Slot2");
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)server_container());
    lua_setfield(L, -2, "ServerContainer");
    lua_pushinteger(L, (lua_Integer)ecs::count(ecs::Context::Replication));
    lua_setfield(L, -2, "ReplicatedTypes");
    lua_pushboolean(L, bg3le_game_allocator_ready());
    lua_setfield(L, -2, "AllocatorReady");
    return 1;
}

// Whether an entity carries a component at all, so the proxy can report a
// missing component as nil rather than as an error.
int l_entity_has_component(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
    const char* name = engine_name_of(luaL_checkstring(L, 2));
    const auto index = component_index(name);
    if (!index.has_value()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    std::int32_t storageIndex = -1;
    void* storage = nullptr;
    void* component = nullptr;
    bg3le_entity_probe(server_container(), handle,
                       static_cast<std::uint16_t>(*index), &storageIndex,
                       &storage, &component);
    lua_pushboolean(L, component != nullptr);
    return 1;
}

extern "C" std::uint64_t bg3le_uuid_to_handle(void* container,
                                              std::uint16_t mappingIndex,
                                              char const* uuid);

// UUID string -> EntityHandle, via ls::uuid::ToHandleMappingComponent. This is
// what lets Osiris UUIDs, which is all a mod ever has, reach the ECS.
int l_uuid_to_handle(lua_State* L) {
    const char* uuid = luaL_checkstring(L, 1);
    const auto index = ecs::index_of(ecs::Context::Component,
                                     "ls::uuid::ToHandleMappingComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "ls::uuid::ToHandleMappingComponent has no index");
        return 2;
    }

    const std::uint64_t handle = bg3le_uuid_to_handle(
        server_container(), static_cast<std::uint16_t>(*index), uuid);
    if (handle == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "UUID not found in the mapping");
        return 2;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(handle));
    return 1;
}

extern "C" std::uint64_t bg3le_find_entity_with(void* container,
                                                std::uint16_t componentIndex,
                                                std::uint32_t* storagesSeen);

// Reports each step of the walk, so a null says where it stopped rather than
// just that it did.
int l_entity_probe(lua_State* L) {
    const auto index = ecs::index_of(ecs::Context::Component, "eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "eoc::HealthComponent has no index");
        return 2;
    }

    // Default to an entity that actually carries the component, rather than
    // whatever the engine happened to look up last.
    std::uint32_t seen = 0;
    auto handle = static_cast<std::uint64_t>(luaL_optinteger(L, 1, 0));
    bool found = false;
    if (handle == 0) {
        handle = bg3le_find_entity_with(server_container(),
                                        static_cast<std::uint16_t>(*index), &seen);
        found = true;
    }

    std::int32_t storageIndex = -1;
    void* storage = nullptr;
    void* component = nullptr;
    bg3le_entity_probe(server_container(), handle,
                       static_cast<std::uint16_t>(*index), &storageIndex,
                       &storage, &component);

    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)*index);
    lua_setfield(L, -2, "HealthIndex");
    lua_pushinteger(L, (lua_Integer)handle);
    lua_setfield(L, -2, "Handle");
    if (found) {
        lua_pushinteger(L, (lua_Integer)seen);
        lua_setfield(L, -2, "StoragesScanned");
    }
    lua_pushinteger(L, storageIndex);
    lua_setfield(L, -2, "StorageIndex");
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(storage));
    lua_setfield(L, -2, "Storage");
    lua_pushinteger(L, (lua_Integer)reinterpret_cast<std::uintptr_t>(component));
    lua_setfield(L, -2, "Component");

    if (component != nullptr) {
        std::int32_t hp = 0;
        std::int32_t maxHp = 0;
        if (bg3le_entity_health(server_container(), handle,
                                static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
            lua_pushinteger(L, hp);
            lua_setfield(L, -2, "Hp");
            lua_pushinteger(L, maxHp);
            lua_setfield(L, -2, "MaxHp");
        }
    }
    return 1;
}

// The most recent EntityHandle the engine looked up, so component access can
// be tested before UUID -> handle exists.
int l_last_entity(lua_State* L) {
    const unsigned long long h = ecs::last_entity();
    if (h == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, static_cast<lua_Integer>(h));
    return 1;
}

// End to end: entity handle -> storage -> component -> field. Uses the
// component index bg3le reads from the symbol table and the container it
// captured, walked by bg3se's own implementation.
int l_entity_health(lua_State* L) {
    const auto handle = static_cast<std::uint64_t>(
        luaL_optinteger(L, 1, static_cast<lua_Integer>(ecs::last_entity())));
    if (handle == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "no entity handle available yet");
        return 2;
    }

    const auto index = ecs::index_of(ecs::Context::Component, "eoc::HealthComponent");
    if (!index.has_value()) {
        lua_pushnil(L);
        lua_pushstring(L, "eoc::HealthComponent has no index yet");
        return 2;
    }

    std::int32_t hp = 0;
    std::int32_t maxHp = 0;
    if (!bg3le_entity_health(server_container(), handle,
                             static_cast<std::uint16_t>(*index), &hp, &maxHp)) {
        lua_pushnil(L);
        lua_pushstring(L, "entity has no Health component");
        return 2;
    }

    lua_newtable(L);
    lua_pushinteger(L, hp);
    lua_setfield(L, -2, "Hp");
    lua_pushinteger(L, maxHp);
    lua_setfield(L, -2, "MaxHp");
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
    lua_pushcfunction(g_lua, l_ecs_dump);
    lua_setfield(g_lua, -2, "EcsDump");
    lua_pushcfunction(g_lua, l_last_entity);
    lua_setfield(g_lua, -2, "LastEntity");
    lua_pushcfunction(g_lua, l_entity_health);
    lua_setfield(g_lua, -2, "EntityHealth");
    lua_pushcfunction(g_lua, l_entity_probe);
    lua_setfield(g_lua, -2, "EntityProbe");
    lua_pushcfunction(g_lua, l_uuid_to_handle);
    lua_setfield(g_lua, -2, "UuidToHandle");
    lua_pushcfunction(g_lua, l_entity_get_health);
    lua_setfield(g_lua, -2, "GetHealth");
    lua_pushcfunction(g_lua, l_entity_set_health);
    lua_setfield(g_lua, -2, "SetHealth");
    lua_pushcfunction(g_lua, l_entity_mark_changed);
    lua_setfield(g_lua, -2, "MarkChanged");
    lua_pushcfunction(g_lua, l_entity_replicate);
    lua_setfield(g_lua, -2, "Replicate");
    lua_pushcfunction(g_lua, l_world_probe);
    lua_setfield(g_lua, -2, "WorldProbe");
    lua_pushcfunction(g_lua, l_get_field);
    lua_setfield(g_lua, -2, "GetField");
    lua_pushcfunction(g_lua, l_set_field);
    lua_setfield(g_lua, -2, "SetField");
    lua_pushcfunction(g_lua, l_component_fields);
    lua_setfield(g_lua, -2, "ComponentFields");
    lua_pushcfunction(g_lua, l_field_info);
    lua_setfield(g_lua, -2, "FieldInfo");
    lua_pushcfunction(g_lua, l_array_info);
    lua_setfield(g_lua, -2, "ArrayInfo");
    lua_pushcfunction(g_lua, l_map_key);
    lua_setfield(g_lua, -2, "MapKey");
    lua_pushcfunction(g_lua, l_field_address);
    lua_setfield(g_lua, -2, "FieldAddress");
    lua_pushcfunction(g_lua, l_field_bytes);
    lua_setfield(g_lua, -2, "FieldBytes");
    lua_pushcfunction(g_lua, l_size_audit);
    lua_setfield(g_lua, -2, "SizeAudit");
    lua_pushcfunction(g_lua, l_entity_has_component);
    lua_setfield(g_lua, -2, "HasComponent");
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

    -- Walked once, keeping the values, rather than collecting keys and
    -- indexing them back. On a component view every index is a read from the
    -- game, so re-indexing would double the work -- and it would bypass the
    -- view's __pairs, which is what turns a field of an unconvertible kind
    -- into a marker rather than an error.
    local items = {}
    local n = 0
    local array = true
    for k, val in pairs(v) do
      n = n + 1
      items[n] = {k = k, v = val}
      if type(k) ~= "number" then array = false end
    end
    array = array and n == #v

    local pad = indent .. "    "
    if n == 0 then
      out[#out+1] = array and "[]" or "{}"
    elseif array then
      table.sort(items, function(a, b) return a.k < b.k end)
      out[#out+1] = "[\n"
      for i = 1, n do
        out[#out+1] = pad
        encode(items[i].v, pad, depth + 1, opts, seen, out)
        out[#out+1] = (i < n) and ",\n" or "\n"
      end
      out[#out+1] = indent .. "]"
    else
      table.sort(items, function(a, b) return tostring(a.k) < tostring(b.k) end)
      out[#out+1] = "{\n"
      for i = 1, n do
        out[#out+1] = pad .. string.format("%q", tostring(items[i].k)) .. ": "
        encode(items[i].v, pad, depth + 1, opts, seen, out)
        out[#out+1] = (i < n) and ",\n" or "\n"
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


-- ---- Ext.Entity ----
--
-- Components are reached through bg3se's own field metadata rather than
-- through an accessor written per component, so every component it describes
-- is available by name with nothing listed here. Either name works: bg3se's
-- short one (entity.Health) or the engine's (entity["eoc::HealthComponent"]).
--
-- Reads and writes go straight to the component in place, so a value is never
-- stale and a write is visible to the next read. The proxy holds only the
-- handle, the name and the field table.
--
-- Naming a field that does not exist raises rather than reading as nil, and so
-- does one whose kind bg3le cannot convert yet. A mod guarding on
-- "if not entity.Foo.Bar then return end" would otherwise skip work and look
-- as though it had succeeded. Ext._Internal.ComponentFields(name) reports what
-- a component offers and the kind of each field.

-- An array field is a view on the component, not a copy of it.
--
-- Returning a plain table would read correctly and then swallow writes:
-- "component.Field[i] = v" would mutate a temporary that is discarded, with
-- nothing to notice, which is the one outcome worse than raising. So elements
-- are read and written through to the component one at a time.
--
-- Elements are reached by extending the path -- "Events[0].Amount" -- rather
-- than by holding an address, so a dynamic array works the same way a fixed
-- one does even though its elements live behind the container's buffer
-- pointer. It also means an element that is itself a struct is just a longer
-- path, so nothing here has to know about nesting.
--
-- One-based, as Lua is. Where the engine indexes by an enum whose first value
-- is None = 0, element 1 is that None slot; that offset is the engine's, and
-- bg3se presents it the same way.
--
-- The length is re-read on every access rather than captured, because a
-- dynamic array can grow or shrink between one access and the next.
--
-- __len and __pairs are defined because Ext.Json.Stringify uses # and pairs,
-- and both honour metamethods -- so a view still dumps like an array.
local make_fields
local make_map

local function make_array(handle, comp, path)
  -- Raises rather than reporting zero, for the reason in make_map below: a
  -- failure to resolve must not read as an empty array.
  local function length()
    local count, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if count == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    return count
  end

  local function element_path(i)
    local count = length()
    if type(i) ~= "number" or i < 1 or i > count then
      error("bg3le: " .. comp .. "." .. path .. " index " .. tostring(i)
            .. " is out of range 1.." .. count, 0)
    end
    return path .. "[" .. (i - 1) .. "]"
  end

  local function element(i)
    local ipath = element_path(i)
    local _, elemKind = Ext._Internal.ArrayInfo(handle, comp, path)
    if elemKind == "struct" then
      local inner, err = Ext._Internal.ComponentFields(comp, ipath)
      if inner == nil then error("bg3le: " .. tostring(err), 0) end
      return make_fields(handle, comp, ipath, inner)
    end
    local value, err = Ext._Internal.GetField(handle, comp, ipath)
    if value == nil and err ~= nil then error("bg3le: " .. err, 0) end
    return value
  end

  return setmetatable({}, {
    __index = function(_, i) return element(i) end,
    __newindex = function(_, i, v)
      local ok, err = Ext._Internal.SetField(handle, comp, element_path(i), v)
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    __len = length,
    __pairs = function(self)
      return function(_, k)
        local i = (k or 0) + 1
        if i > length() then return nil end
        return i, element(i)
      end, self, nil
    end,
  })
end

-- A map field is a view too, keyed the way the engine keys it.
--
-- The engine keeps a hash map's keys and values in two parallel runs, so slot
-- i holds key i alongside value i. That is what makes this presentable without
-- hashing anything from Lua: iterating is a walk over both runs, and a lookup
-- is that walk plus a comparison. Lookup is therefore linear rather than
-- hashed, which is fine for the maps on a component -- they hold a handful of
-- entries -- and it avoids needing the engine's own hash for every key type.
--
-- Keys of a kind bg3le cannot convert -- a FixedString, which would need the
-- engine's global string table -- leave the key side unavailable while the
-- values stay reachable by slot, which is what Entries() is for.
make_map = function(handle, comp, path)
  -- Raises rather than reporting zero. "or 0" here turned any failure to
  -- resolve into an empty map, which is the worst possible answer: a mod sees
  -- a container that is present and empty, and there is nothing to notice.
  -- It hid a real discrepancy in SummonContainer.ByTag for exactly as long as
  -- it took to compare against bg3se on Windows.
  local function count()
    local n, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if n == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    return n
  end

  local function key_at(i)
    return Ext._Internal.MapKey(handle, comp, path, i)
  end

  local function value_at(i)
    local vpath = path .. "[" .. i .. "]"
    local kind = Ext._Internal.FieldInfo(comp, vpath)
    if kind == "struct" then
      local inner, err = Ext._Internal.ComponentFields(comp, vpath)
      if inner == nil then error("bg3le: " .. tostring(err), 0) end
      return make_fields(handle, comp, vpath, inner)
    end
    if kind == "array" then return make_array(handle, comp, vpath) end
    if kind == "map" then return make_map(handle, comp, vpath) end
    local value, err = Ext._Internal.GetField(handle, comp, vpath)
    if value == nil and err ~= nil then error("bg3le: " .. err, 0) end
    return value
  end

  -- Slot of a key, or nil. Linear, as above.
  local function slot_of(key)
    for i = 0, count() - 1 do
      if key_at(i) == key then return i end
    end
    return nil
  end

  return setmetatable({}, {
    __index = function(_, key)
      -- A method rather than a field, so a map whose keys cannot be converted
      -- is still walkable.
      if key == "Entries" then
        return function()
          local out = {}
          for i = 0, count() - 1 do
            out[i + 1] = {Key = key_at(i), Value = value_at(i)}
          end
          return out
        end
      end
      local i = slot_of(key)
      if i == nil then return nil end
      return value_at(i)
    end,
    __newindex = function(_, key, value)
      local i = slot_of(key)
      if i == nil then
        error("bg3le: " .. comp .. "." .. path .. " has no key "
              .. tostring(key) .. "; adding one is not supported", 0)
      end
      local ok, err = Ext._Internal.SetField(
        handle, comp, path .. "[" .. i .. "]", value)
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    __len = count,
    -- Iterating yields key, value.
    --
    -- A key that cannot be converted yields a placeholder rather than ending
    -- the iteration. Ending it made a map that holds entries render as {},
    -- which is indistinguishable from an empty one: SummonContainer.ByTag
    -- holds two tagged entries whose FixedString keys bg3le cannot read, and
    -- it dumped as empty. The values are perfectly reachable, so hiding them
    -- was the worst of the options -- a placeholder is visible, stopping was
    -- not.
    __pairs = function(self)
      local i = -1
      return function()
        i = i + 1
        if i >= count() then return nil end
        local k = key_at(i)
        if k == nil then k = "<unreadable key " .. i .. ">" end
        return k, value_at(i)
      end, self, nil
    end,
  })
end

-- A view over a set of fields, used for a component, for a struct nested
-- inside one, and for a struct that is an array element; the only difference
-- is the path prefix.
make_fields = function(handle, comp, prefix, fields)
  local function path_to(key)
    if prefix == "" then return key end
    return prefix .. "." .. key
  end

  return setmetatable({}, {
    __index = function(_, key)
      local kind = fields[key]
      if kind == nil then
        local where = prefix == "" and comp or (comp .. "." .. prefix)
        error("bg3le: " .. where .. " has no field " .. tostring(key), 0)
      end
      local path = path_to(key)
      if kind == "array" then
        return make_array(handle, comp, path)
      end
      if kind == "map" then
        return make_map(handle, comp, path)
      end
      if kind == "struct" then
        local inner, err = Ext._Internal.ComponentFields(comp, path)
        if inner == nil then error("bg3le: " .. tostring(err), 0) end
        return make_fields(handle, comp, path, inner)
      end
      local value, err = Ext._Internal.GetField(handle, comp, path)
      if value == nil and err ~= nil then error("bg3le: " .. err, 0) end
      return value
    end,
    __newindex = function(_, key, value)
      local ok, err = Ext._Internal.SetField(handle, comp, path_to(key), value)
      if not ok then error("bg3le: " .. tostring(err), 0) end
    end,
    -- Iterating yields field names and their values, so dumping a component
    -- shows what it holds. A field of a kind bg3le cannot convert yields the
    -- marker string instead of raising the way direct access does -- a dump
    -- has to be able to walk the whole component, and Ext.Json.Stringify
    -- reads each key back through __index, so raising there would make any
    -- component with one unconvertible field undumpable. The marker is a
    -- string rather than nil for the usual reason: nil would read as absent.
    __pairs = function(self)
      local key
      return function()
        local kind
        key, kind = next(fields, key)
        if key == nil then return nil end
        if kind == "unsupported" then return key, "<unsupported>" end
        local ok, value = pcall(function() return self[key] end)
        if not ok then return key, "<unreadable>" end
        return key, value
      end, self, nil
    end,
  })
end

local function make_component(handle, name, fields)
  return make_fields(handle, name, "", fields)
end

-- nil if bg3se has no metadata for the name, or if this entity does not carry
-- the component. Those are different answers, but both mean "not available
-- here", which is what a script branches on.
local function get_component(handle, name)
  local fields = Ext._Internal.ComponentFields(name)
  if fields == nil then return nil end
  if not Ext._Internal.HasComponent(handle, name) then return nil end
  return make_component(handle, name, fields)
end

local entity_methods = {}

function entity_methods:GetComponent(name)
  return get_component(self.Handle, name)
end

-- Marking the component changed is what the server acts on; setting the
-- replication flags is what reaches the client. Both are needed for a write
-- to show up in the UI.
function entity_methods:Replicate(name)
  if not Ext._Internal.MarkChanged(self.Handle, name) then
    error("bg3le: could not mark " .. tostring(name) .. " as changed", 0)
  end
  local ok, err = Ext._Internal.Replicate(self.Handle, name)
  if not ok then error("bg3le: " .. tostring(err), 0) end
  return true
end

local entity_meta = {
  __index = function(entity, key)
    local method = entity_methods[key]
    if method ~= nil then return method end
    return get_component(rawget(entity, "Handle"), key)
  end,
}

Ext.Entity = {}

-- Accepts a UUID string, as mods do, or a raw EntityHandle.
function Ext.Entity.Get(id)
  local handle
  if type(id) == "string" then
    handle = Ext._Internal.UuidToHandle(id)
    if handle == nil then return nil end
  elseif type(id) == "number" then
    handle = id
  else
    return nil
  end

  return setmetatable({Handle = handle,
                       EntityUuid = type(id) == "string" and id or nil},
                      entity_meta)
end

function Ext.Entity.HandleToUuid(handle)
  return Ext._Internal.GetField(handle, "Uuid", "EntityUuid")
end

function Ext.Entity.UuidToHandle(uuid) return Ext._Internal.UuidToHandle(uuid) end

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
