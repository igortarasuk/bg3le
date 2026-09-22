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
#include "vendor/mods.h"

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
extern "C" bool bg3le_meta_component_is_one_frame(void const* handle);
extern "C" void* bg3le_entity_one_frame_component(void* container,
                                                  std::uint64_t handle,
                                                  std::uint16_t componentIndex);
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
extern "C" bool bg3le_meta_variant_index(void const* handle,
                                         const char* path, void* component,
                                         std::size_t* active,
                                         std::size_t* count);
extern "C" bool bg3le_meta_map_key(void const* handle, const char* path,
                                   void* component, std::size_t index,
                                   void** address, std::uint8_t* kind,
                                   std::uint16_t* size);
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);
// From src/vendor/fixed_string.cpp.
extern "C" const char* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" void* bg3le_string_table();
extern "C" bool bg3le_meta_enum_label(void const* handle, const char* path,
                                      std::size_t index, const char** label,
                                      std::uint64_t* value, bool* isBitmask);
extern "C" std::size_t bg3le_meta_enum_count();
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

    // A one-frame component's engine index carries 0x8000, which is what
    // ecs::IsOneFrame tests for. The two registries are numbered
    // independently, so without the flag a one-frame index silently collides
    // with an unrelated inline component -- which is what put 17 of them in
    // SizeAudit's mismatch list. Those were never size disagreements; they
    // were comparisons against whichever inline component shared the number.
    if (auto i = ecs::index_of(ecs::Context::OneFrameComponent, name)) {
        return *i | 0x8000;
    }
    return std::nullopt;
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
    Int32, Uint32, Int64, Uint64, Guid, Entity, FixedString, ScalarArray,
    Struct, DynArray, Map, Optional, Variant, Inherit,
};

extern "C" const char* bg3le_meta_kind_name(std::uint8_t kind);

// Named in one place, in src/vendor/component_meta.cpp, because these were
// duplicated and inserting a kind mid-enum renumbered everything after it.
const char* field_kind_name(FieldKind kind) {
    return bg3le_meta_kind_name((std::uint8_t)kind);
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
        case FieldKind::FixedString:
            return 4;
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

    // A one-frame component is not in the entity page at all: the engine keeps
    // it in a per-storage pool keyed by entity. Reading one through the page
    // returns whatever is at that offset, which is what the one-frame entries
    // in SizeAudit were -- not a wrong struct, a wrong mechanism.
    if (bg3le_meta_component_is_one_frame(*meta)) {
        return bg3le_entity_one_frame_component(
            server_container(), handle, static_cast<std::uint16_t>(*index));
    }

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
        case FieldKind::FixedString: {
            // The index is meaningless on its own, so an unresolved one is
            // reported rather than pushed as a number: a bare integer would
            // read as a value and it is not one.
            std::uint32_t index = 0;
            if (!safe_read(address, &index, 4)) return false;
            if (index == 0xffffffffu) {
                lua_pushnil(L);
                return true;
            }
            std::uint32_t length = 0;
            const char* text = bg3le_fixed_string(index, &length);
            if (text == nullptr) {
                lua_pushfstring(L,
                    bg3le_string_table() == nullptr
                        ? "<string table not found>"
                        : "<unresolved string %d>", (int)index);
                return true;
            }
            char buf[513];
            if (length > sizeof(buf) - 1) length = sizeof(buf) - 1;
            if (!safe_read(text, buf, length)) {
                lua_pushstring(L, "<unreadable string>");
                return true;
            }
            lua_pushlstring(L, buf, length);
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

// Pushes an enum-typed field as its label, or a bitmask as the list of set
// flags -- which is how bg3se presents them, and scripts are written against
// that. Returns false if the field is not an enum, leaving the caller to push
// the raw integer.
//
// A value with no matching label is pushed as the number, so an unmapped bit
// is visible rather than dropped.
bool push_enum(lua_State* L, void const* meta, const char* path,
               std::uint64_t raw) {
    const char* label = nullptr;
    std::uint64_t value = 0;
    bool isBitmask = false;
    if (!bg3le_meta_enum_label(meta, path, 0, &label, &value, &isBitmask)) {
        return false;
    }

    if (!isBitmask) {
        for (std::size_t i = 0;
             bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask);
             ++i) {
            if (value == raw) {
                lua_pushstring(L, label);
                return true;
            }
        }
        lua_pushinteger(L, (lua_Integer)raw);
        return true;
    }

    lua_newtable(L);
    int n = 0;
    std::uint64_t matched = 0;
    for (std::size_t i = 0;
         bg3le_meta_enum_label(meta, path, i, &label, &value, &isBitmask);
         ++i) {
        if (value != 0 && (raw & value) == value) {
            lua_pushstring(L, label);
            lua_rawseti(L, -2, ++n);
            matched |= value;
        }
    }
    // Any bits the table does not describe are reported as a leftover number,
    // so a flag the metadata is missing does not vanish.
    if ((raw & ~matched) != 0) {
        lua_pushinteger(L, (lua_Integer)(raw & ~matched));
        lua_rawseti(L, -2, ++n);
    }
    return true;
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

    // An enum keeps its underlying integer kind, so it is read as a number
    // and then rendered as a label.
    const std::size_t width = field_kind_size((FieldKind)kind);
    if (width != 0 && width <= 8) {
        std::uint64_t raw = 0;
        if (safe_read(address, &raw, width)
            && push_enum(L, meta, path, raw)) {
            return 1;
        }
    }

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
    // The element kind as it actually is, which for anything that is not a
    // scalar is "unsupported": the field tables only record a scalar kind for
    // elements. This used to answer "struct" instead, reasoning that a
    // non-scalar element must be one -- and then an element that was itself a
    // container got walked as a struct and raised. Nothing dispatches on this
    // any more, since read_path asks about the element's own path, so it can
    // simply be accurate.
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    return 2;
}

extern "C" void const* bg3le_meta_class(const char* className);
extern "C" bool bg3le_meta_parse_guid(const char* text, void* out);
extern "C" void* bg3le_resource_get(std::int32_t typeIndex, void const* guid,
                                    std::size_t resourceSize);
extern "C" std::size_t bg3le_resource_count(std::int32_t typeIndex);
extern "C" bool bg3le_resource_guid_at(std::int32_t typeIndex, std::size_t i,
                                       void* guidOut);
// Ext.Stats. The manager is found by fingerprint like the resource manager;
// see src/vendor/stats.cpp for why the attribute values need four lookups
// rather than an offset.
// Ext.Mod. The module list is found from the base module's constant
// uuid; see src/vendor/mods.cpp.
extern "C" std::size_t bg3le_mods_count();
extern "C" char const* bg3le_mods_uuid_at(std::size_t index);
extern "C" void* bg3le_mods_at(std::size_t index);
extern "C" void* bg3le_mods_find(char const* uuid);
extern "C" void* bg3le_mods_base();
extern "C" std::size_t bg3le_mods_available_count();
extern "C" bool bg3le_stat_origin(char const* name, char const** modId,
                                  char const** originalModId);
extern "C" void* bg3le_mods_available_at(std::size_t index);
extern "C" bool bg3le_mod_info(void const* module, bg3le::ModInfo* out);
extern "C" std::size_t bg3le_mod_list_count(void const* module, int list);
extern "C" bool bg3le_mod_list_at(void const* module, int list,
                                  std::size_t index, bg3le::ModShortDesc* out);

extern "C" void* bg3le_stats_manager();
extern "C" std::size_t bg3le_stats_count();
extern "C" void* bg3le_stats_at(std::size_t index);
extern "C" char const* bg3le_stats_name(void const* object);
extern "C" void* bg3le_stats_find(char const* name);
extern "C" char const* bg3le_stats_type(void const* object);
extern "C" char const* bg3le_stats_using(void const* object);
extern "C" int bg3le_stats_list_index(void const* object);
extern "C" std::size_t bg3le_stats_attr_count(void const* object);
extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut,
                                    char const** typeNameOut, int* kindOut,
                                    int* rawOut);
extern "C" char const* bg3le_stats_attr_label(void const* object,
                                              std::size_t index, int raw);
extern "C" char const* bg3le_stats_attr_string(int raw);
extern "C" bool bg3le_stats_attr_float(int raw, double* out);
extern "C" bool bg3le_stats_attr_guid(int raw, char* out,
                                      std::size_t capacity);
extern "C" bool bg3le_stats_attr_flags(void const* object,
                                       std::size_t index, int raw,
                                       char* out,
                                       std::size_t capacity);

extern "C" void* bg3le_resource_manager();
extern "C" std::size_t bg3le_resource_bank_count();
extern "C" bool bg3le_resource_bank_at(std::size_t i, std::int32_t* typeIndex,
                                       void** bank);

// A field call's subject: a class, and the address its fields are relative to.
//
// The field machinery never needed an entity -- only these two. An entity and
// a component resolve to a subject; a static data resource already is one,
// which is what makes the same code serve both without a second copy of it.
struct Subject {
    void const* Meta{nullptr};
    void* Base{nullptr};
};

// From an address and a class name, as a static data resource arrives.
bool subject_from_object(lua_State* L, int addressIdx, int classIdx,
                         Subject* out, const char** className) {
    *className = luaL_checkstring(L, classIdx);
    out->Base = (void*)(std::uintptr_t)luaL_checkinteger(L, addressIdx);
    out->Meta = bg3le_meta_class(*className);
    if (out->Meta == nullptr || out->Base == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for class %s", *className);
        return false;
    }
    return true;
}

// Ext._Internal.ObjectFields(class [, path]) -> { field = kind, ... }
int l_object_fields(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* path = luaL_optstring(L, 2, nullptr);

    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no field metadata for class %s", className);
        return 2;
    }

    constexpr std::size_t kMax = 512;
    const char* names[kMax];
    std::uint8_t kinds[kMax];
    const std::size_t count =
        bg3le_meta_fields_at(meta, path, names, kinds, kMax);
    if (count == 0 && path != nullptr && path[0] != '\0') {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s cannot be traversed", className, path);
        return 2;
    }

    lua_newtable(L);
    for (std::size_t i = 0; i < count; ++i) {
        lua_pushstring(L, field_kind_name((FieldKind)kinds[i]));
        lua_setfield(L, -2, names[i]);
    }
    return 1;
}

// Ext._Internal.ObjectFieldInfo(class, path) -> kind, elemKind, elemCount
int l_object_field_info(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* path = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_class(className);
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

// Ext._Internal.ObjectGetField(address, class, path)
int l_object_get_field(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(subject.Meta, path, subject.Base, &address, &kind,
                            &size, &readOnly)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s does not resolve", className, path);
        return 2;
    }

    std::uint32_t fieldOffset = 0;
    std::uint16_t fieldSize = 0;
    std::uint8_t fieldKind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    bg3le_meta_field(subject.Meta, path, &fieldOffset, &fieldSize, &fieldKind,
                     &elemKind, &elemCount);

    const std::size_t width = field_kind_size((FieldKind)kind);
    if (width != 0 && width <= 8) {
        std::uint64_t raw = 0;
        if (safe_read(address, &raw, width)
            && push_enum(L, subject.Meta, path, raw)) {
            return 1;
        }
    }

    if (!push_field(L, address, (FieldKind)kind, (FieldKind)elemKind,
                    elemCount)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is of an unsupported kind (%s)", className,
                        path, field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

// Ext._Internal.ObjectArrayInfo(address, class, path) -> count, elementKind
int l_object_array_info(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    const int status = bg3le_meta_array_length(subject.Meta, path, subject.Base,
                                               &count, &elemSize, &elemKind);
    if (status != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "cannot size %s.%s (status %d)", className, path,
                        status);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)count);
    lua_pushstring(L, field_kind_name((FieldKind)elemKind));
    return 2;
}

// Ext._Internal.ObjectVariantIndex(address, class, path) -> active, count
int l_object_variant_index(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);

    std::size_t active = 0;
    std::size_t count = 0;
    if (!bg3le_meta_variant_index(subject.Meta, path, subject.Base, &active,
                                  &count)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a variant", className, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)active);
    lua_pushinteger(L, (lua_Integer)count);
    return 2;
}

// Ext._Internal.ObjectMapKey(address, class, path, index) -> key
int l_object_map_key(lua_State* L) {
    Subject subject;
    const char* className = nullptr;
    if (!subject_from_object(L, 1, 2, &subject, &className)) return 2;
    const char* path = luaL_checkstring(L, 3);
    const auto index = (std::size_t)luaL_checkinteger(L, 4);

    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    if (!bg3le_meta_map_key(subject.Meta, path, subject.Base, index, &address,
                            &kind, &size)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s has no key at slot %d", className, path,
                        (int)index);
        return 2;
    }

    if (!push_field(L, address, (FieldKind)kind)) {
        lua_pushnil(L);
        lua_pushfstring(L, "the keys of %s.%s are of an unsupported kind (%s)",
                        className, path, field_kind_name((FieldKind)kind));
        return 2;
    }
    return 1;
}

// Ext._Internal.ResourceGet(class, guid) -> address
//
// The chain: the class names the resource type, its EngineClass names the
// static data type, the symbol table gives that type's index, the manager
// gives the bank for the index, and the bank maps the GUID to the resource.
// Every link but the manager was already in place.
int l_resource_get(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);
    const char* guidText = luaL_checkstring(L, 2);

    void const* meta = bg3le_meta_class(className);
    if (meta == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no metadata for resource class %s", className);
        return 2;
    }

    const char* engineClass = bg3le_meta_engine_class(meta);
    if (engineClass == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s has no engine class, so no static data type",
                        className);
        return 2;
    }

    const auto typeIndex =
        ecs::index_of(ecs::Context::ImmutableData, engineClass);
    if (!typeIndex.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a registered static data type",
                        engineClass);
        return 2;
    }

    std::uint8_t guid[16];
    if (!bg3le_meta_parse_guid(guidText, guid)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a GUID", guidText);
        return 2;
    }

    void* resource =
        bg3le_resource_get(*typeIndex, guid, bg3le_meta_component_size(meta));
    if (resource == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no %s with that GUID; the bank holds %d",
                        className, (int)bg3le_resource_count(*typeIndex));
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)resource);
    return 1;
}

// Ext._Internal.ResourceGuids(class) -> { guid, ... }
int l_resource_guids(lua_State* L) {
    const char* className = luaL_checkstring(L, 1);

    void const* meta = bg3le_meta_class(className);
    const char* engineClass =
        meta != nullptr ? bg3le_meta_engine_class(meta) : nullptr;
    if (engineClass == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no static data type for %s", className);
        return 2;
    }

    const auto typeIndex =
        ecs::index_of(ecs::Context::ImmutableData, engineClass);
    if (!typeIndex.has_value()) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s is not a registered static data type",
                        engineClass);
        return 2;
    }

    const std::size_t count = bg3le_resource_count(*typeIndex);
    lua_createtable(L, (int)count, 0);
    int written = 0;
    for (std::size_t i = 0; i < count; ++i) {
        std::uint8_t guid[16];
        if (!bg3le_resource_guid_at(*typeIndex, i, guid)) continue;
        char text[40];
        if (!bg3le_meta_format_guid(guid, text, sizeof(text))) continue;
        lua_pushstring(L, text);
        lua_rawseti(L, -2, ++written);
    }
    return 1;
}

// ---- Ext.Mod ----

// Ext._Internal.ModCount() -> n
int l_mod_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_mods_count());
    return 1;
}

// Ext._Internal.ModUuidAt(index) -> uuid string
int l_mod_uuid_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    char const* uuid = bg3le_mods_uuid_at(i);
    if (uuid == nullptr) return 0;
    lua_pushstring(L, uuid);
    return 1;
}

// Ext._Internal.ModAt(index) -> address, and ModFind(uuid) -> address.
// Ext.Mod takes the module by address so a lookup is paid for once.
int l_mod_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* module = bg3le_mods_at(i);
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

int l_mod_find(lua_State* L) {
    void* module = bg3le_mods_find(luaL_checkstring(L, 1));
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

// Ext._Internal.ModAvailableCount() / ModAvailableAt(index)
int l_mod_available_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_mods_available_count());
    return 1;
}

int l_mod_available_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* module = bg3le_mods_available_at(i);
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

int l_mod_base(lua_State* L) {
    void* module = bg3le_mods_base();
    if (module == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)module);
    return 1;
}

void push_version(lua_State* L, std::uint32_t const v[4]) {
    lua_createtable(L, 4, 0);
    for (int i = 0; i < 4; ++i) {
        lua_pushinteger(L, (lua_Integer)v[i]);
        lua_rawseti(L, -2, i + 1);
    }
}

void set_string(lua_State* L, char const* key, char const* value) {
    lua_pushstring(L, value != nullptr ? value : "");
    lua_setfield(L, -2, key);
}

// Ext._Internal.ModInfo(address) -> the ModuleInfo table
//
// Keys and shapes follow reference/mod-shape.txt exactly; a mod written
// against bg3se reads mod.Info.Directory and has to find it here.
int l_mod_info(lua_State* L) {
    auto const* module =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    bg3le::ModInfo info{};
    if (!bg3le_mod_info(module, &info)) return 0;

    lua_createtable(L, 0, 16);
    set_string(L, "ModuleUUID", info.ModuleUUIDString);
    set_string(L, "ModuleUUIDString", info.ModuleUUIDString);
    set_string(L, "Name", info.Name);
    set_string(L, "Directory", info.Directory);
    set_string(L, "Hash", info.Hash);
    set_string(L, "Author", info.Author);
    set_string(L, "Description", info.Description);
    set_string(L, "StartLevelName", info.StartLevelName);
    set_string(L, "MenuLevelName", info.MenuLevelName);
    set_string(L, "LobbyLevelName", info.LobbyLevelName);
    set_string(L, "CharacterCreationLevelName",
               info.CharacterCreationLevelName);
    set_string(L, "PhotoBoothLevelName", info.PhotoBoothLevelName);

    push_version(L, info.ModVersion);
    lua_setfield(L, -2, "ModVersion");
    push_version(L, info.PublishVersion);
    lua_setfield(L, -2, "PublishVersion");

    lua_pushinteger(L, (lua_Integer)info.NumPlayers);
    lua_setfield(L, -2, "NumPlayers");
    lua_pushinteger(L, (lua_Integer)info.FileSize);
    lua_setfield(L, -2, "FileSize");
    lua_pushinteger(L, (lua_Integer)info.PublishHandle);
    lua_setfield(L, -2, "PublishHandle");
    return 1;
}

// Ext._Internal.ModList(address, which) -> { ModuleShortDesc, ... }
// which is 0 Dependencies, 1 ModConflicts, 2 Addons.
int l_mod_list(lua_State* L) {
    auto const* module =
        (void const*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const int which = (int)luaL_checkinteger(L, 2);

    const std::size_t n = bg3le_mod_list_count(module, which);
    lua_createtable(L, (int)n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        bg3le::ModShortDesc desc{};
        if (!bg3le_mod_list_at(module, which, i, &desc)) break;

        lua_createtable(L, 0, 7);
        set_string(L, "ModuleUUID", desc.ModuleUUIDString);
        set_string(L, "ModuleUUIDString", desc.ModuleUUIDString);
        set_string(L, "Name", desc.Name);
        set_string(L, "Folder", desc.Folder);
        set_string(L, "Hash", desc.Hash);
        push_version(L, desc.ModVersion);
        lua_setfield(L, -2, "ModVersion");
        push_version(L, desc.PublishVersion);
        lua_setfield(L, -2, "PublishVersion");
        lua_pushinteger(L, (lua_Integer)desc.PublishHandle);
        lua_setfield(L, -2, "PublishHandle");

        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}

// Ext._Internal.StatOrigin(name) -> modId, originalModId
//
// Not a field on the stat: which mod defines an entry comes from the
// archives. See src/vendor/stat_origins.cpp.
int l_stat_origin(lua_State* L) {
    char const* name = luaL_checkstring(L, 1);
    char const* modId = nullptr;
    char const* originalModId = nullptr;
    if (!bg3le_stat_origin(name, &modId, &originalModId)) return 0;

    if (modId != nullptr) {
        lua_pushstring(L, modId);
    } else {
        lua_pushnil(L);
    }
    if (originalModId != nullptr) {
        lua_pushstring(L, originalModId);
    } else {
        lua_pushnil(L);
    }
    return 2;
}

// ---- Ext.Stats ----
//
// The manager is found by fingerprint (src/vendor/stats.cpp). Attribute
// values are stored apart from their names, so one attribute takes a name, a
// type and a raw int, and the decoding of that int depends on the type. These
// entry points hand the pieces to Lua and the prelude assembles them, which
// keeps the C side free of policy about how a stat should look.

// Ext._Internal.StatsCount() -> n
int l_stats_count(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)bg3le_stats_count());
    return 1;
}

// Ext._Internal.StatsNameAt(index) -> name
int l_stats_name_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* obj = bg3le_stats_at(i);
    if (obj == nullptr) return 0;
    char const* name = bg3le_stats_name(obj);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    return 1;
}

// Ext._Internal.StatsAt(index) -> address
//
// Filtering needs the address without paying for a name lookup: StatsFind is
// a linear scan, so using it per stat would be quadratic.
int l_stats_at(lua_State* L) {
    const auto i = (std::size_t)luaL_checkinteger(L, 1);
    void* obj = bg3le_stats_at(i);
    if (obj == nullptr) return 0;
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)obj);
    return 1;
}

// Ext._Internal.StatsFind(name) -> address
int l_stats_find(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    void* obj = bg3le_stats_find(name);
    if (obj == nullptr) {
        lua_pushnil(L);
        lua_pushfstring(L, "no stat named %s (the manager holds %d)", name,
                        (int)bg3le_stats_count());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)obj);
    return 1;
}

// Ext._Internal.StatsType(address) -> modifier list name
int l_stats_type(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* type = bg3le_stats_type(obj);
    if (type == nullptr) return 0;
    lua_pushstring(L, type);
    return 1;
}

// Ext._Internal.StatsUsing(address) -> parent stat name
int l_stats_using(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    char const* name = bg3le_stats_using(obj);
    if (name == nullptr) return 0;
    lua_pushstring(L, name);
    return 1;
}

// Ext._Internal.StatsListIndex(address) -> index
int l_stats_list_index(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, bg3le_stats_list_index(obj));
    return 1;
}

// Ext._Internal.StatsAttrCount(address) -> n
int l_stats_attr_count(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    lua_pushinteger(L, (lua_Integer)bg3le_stats_attr_count(obj));
    return 1;
}

// Ext._Internal.StatsAttrAt(address, index) -> name, typeName, kind, raw
int l_stats_attr_at(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);

    char const* name = nullptr;
    char const* typeName = nullptr;
    int kind = 0;
    int raw = 0;
    if (!bg3le_stats_attr_at(obj, i, &name, &typeName, &kind, &raw)) {
        return 0;
    }

    if (name != nullptr) lua_pushstring(L, name);
    else lua_pushnil(L);
    if (typeName != nullptr) lua_pushstring(L, typeName);
    else lua_pushnil(L);
    lua_pushinteger(L, kind);
    lua_pushinteger(L, raw);
    return 4;
}

// Ext._Internal.StatsAttrLabel(address, index, raw) -> label
int l_stats_attr_label(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);
    const int raw = (int)luaL_checkinteger(L, 3);
    char const* label = bg3le_stats_attr_label(obj, i, raw);
    if (label == nullptr) return 0;
    lua_pushstring(L, label);
    return 1;
}

// Ext._Internal.StatsAttrString(raw) -> text
int l_stats_attr_string(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    char const* text = bg3le_stats_attr_string(raw);
    if (text == nullptr) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsAttrFloat(raw) -> number
int l_stats_attr_float(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    double v = 0.0;
    if (!bg3le_stats_attr_float(raw, &v)) return 0;
    lua_pushnumber(L, v);
    return 1;
}

// Ext._Internal.StatsAttrFlags(address, index, raw) -> "A;B;C"
int l_stats_attr_flags(lua_State* L) {
    auto* obj = (void*)(std::uintptr_t)luaL_checkinteger(L, 1);
    const auto i = (std::size_t)luaL_checkinteger(L, 2);
    const int raw = (int)luaL_checkinteger(L, 3);
    char text[1024];
    if (!bg3le_stats_attr_flags(obj, i, raw, text, sizeof(text))) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.StatsAttrGuid(raw) -> guid string
int l_stats_attr_guid(lua_State* L) {
    const int raw = (int)luaL_checkinteger(L, 1);
    char text[40];
    if (!bg3le_stats_attr_guid(raw, text, sizeof(text))) return 0;
    lua_pushstring(L, text);
    return 1;
}

// Ext._Internal.ResourceBanks() -> { Manager, Banks = { {...}, ... } }
//
// Every bank the resource manager holds, with the name the symbol table gives
// its type index. That naming is the check on the search: the manager is found
// by looking for a table whose keys are all static data indices bg3le already
// knows, so if the names come back as real resource types rather than as gaps,
// the structure found is the right one.
int l_resource_banks(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)(std::uintptr_t)bg3le_resource_manager());
    lua_setfield(L, -2, "Manager");
    lua_pushinteger(L, (lua_Integer)ecs::count(ecs::Context::ImmutableData));
    lua_setfield(L, -2, "RegisteredTypes");

    const std::size_t count = bg3le_resource_bank_count();
    lua_createtable(L, (int)count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::int32_t typeIndex = 0;
        void* bank = nullptr;
        if (!bg3le_resource_bank_at(i, &typeIndex, &bank)) break;

        lua_newtable(L);
        lua_pushinteger(L, typeIndex);
        lua_setfield(L, -2, "TypeIndex");
        const auto name = ecs::name_of(ecs::Context::ImmutableData, typeIndex);
        if (name.has_value()) {
            lua_pushstring(L, name->c_str());
        } else {
            lua_pushstring(L, "<not in the registry>");
        }
        lua_setfield(L, -2, "Name");
        lua_pushinteger(L, (lua_Integer)(std::uintptr_t)bank);
        lua_setfield(L, -2, "Bank");
        lua_rawseti(L, -2, (int)i + 1);
    }
    lua_setfield(L, -2, "Banks");
    return 1;
}

// Ext._Internal.VariantIndex(handle, component, path) -> active, count
//
// active is zero-based, and equals count when the variant holds nothing.
// std::variant only reaches that state if a move threw, so it should not
// happen -- but it is representable, so it is reported rather than conflated
// with holding alternative zero.
int l_variant_index(lua_State* L) {
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

    std::size_t active = 0;
    std::size_t count = 0;
    if (!bg3le_meta_variant_index(meta, path, component, &active, &count)) {
        lua_pushnil(L);
        lua_pushfstring(L, "%s.%s is not a variant", name, path);
        return 2;
    }

    lua_pushinteger(L, (lua_Integer)active);
    lua_pushinteger(L, (lua_Integer)count);
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
    lua_pushcfunction(g_lua, l_variant_index);
    lua_setfield(g_lua, -2, "VariantIndex");
    lua_pushcfunction(g_lua, l_mod_count);
    lua_setfield(g_lua, -2, "ModCount");
    lua_pushcfunction(g_lua, l_mod_uuid_at);
    lua_setfield(g_lua, -2, "ModUuidAt");
    lua_pushcfunction(g_lua, l_mod_at);
    lua_setfield(g_lua, -2, "ModAt");
    lua_pushcfunction(g_lua, l_mod_find);
    lua_setfield(g_lua, -2, "ModFind");
    lua_pushcfunction(g_lua, l_mod_base);
    lua_setfield(g_lua, -2, "ModBase");
    lua_pushcfunction(g_lua, l_mod_available_count);
    lua_setfield(g_lua, -2, "ModAvailableCount");
    lua_pushcfunction(g_lua, l_mod_available_at);
    lua_setfield(g_lua, -2, "ModAvailableAt");
    lua_pushcfunction(g_lua, l_mod_info);
    lua_setfield(g_lua, -2, "ModInfo");
    lua_pushcfunction(g_lua, l_mod_list);
    lua_setfield(g_lua, -2, "ModList");
    lua_pushcfunction(g_lua, l_stat_origin);
    lua_setfield(g_lua, -2, "StatOrigin");
    lua_pushcfunction(g_lua, l_stats_count);
    lua_setfield(g_lua, -2, "StatsCount");
    lua_pushcfunction(g_lua, l_stats_name_at);
    lua_setfield(g_lua, -2, "StatsNameAt");
    lua_pushcfunction(g_lua, l_stats_at);
    lua_setfield(g_lua, -2, "StatsAt");
    lua_pushcfunction(g_lua, l_stats_find);
    lua_setfield(g_lua, -2, "StatsFind");
    lua_pushcfunction(g_lua, l_stats_type);
    lua_setfield(g_lua, -2, "StatsType");
    lua_pushcfunction(g_lua, l_stats_using);
    lua_setfield(g_lua, -2, "StatsUsing");
    lua_pushcfunction(g_lua, l_stats_list_index);
    lua_setfield(g_lua, -2, "StatsListIndex");
    lua_pushcfunction(g_lua, l_stats_attr_count);
    lua_setfield(g_lua, -2, "StatsAttrCount");
    lua_pushcfunction(g_lua, l_stats_attr_at);
    lua_setfield(g_lua, -2, "StatsAttrAt");
    lua_pushcfunction(g_lua, l_stats_attr_label);
    lua_setfield(g_lua, -2, "StatsAttrLabel");
    lua_pushcfunction(g_lua, l_stats_attr_string);
    lua_setfield(g_lua, -2, "StatsAttrString");
    lua_pushcfunction(g_lua, l_stats_attr_float);
    lua_setfield(g_lua, -2, "StatsAttrFloat");
    lua_pushcfunction(g_lua, l_stats_attr_guid);
    lua_setfield(g_lua, -2, "StatsAttrGuid");
    lua_pushcfunction(g_lua, l_stats_attr_flags);
    lua_setfield(g_lua, -2, "StatsAttrFlags");
    lua_pushcfunction(g_lua, l_resource_banks);
    lua_setfield(g_lua, -2, "ResourceBanks");
    lua_pushcfunction(g_lua, l_resource_get);
    lua_setfield(g_lua, -2, "ResourceGet");
    lua_pushcfunction(g_lua, l_resource_guids);
    lua_setfield(g_lua, -2, "ResourceGuids");
    lua_pushcfunction(g_lua, l_object_fields);
    lua_setfield(g_lua, -2, "ObjectFields");
    lua_pushcfunction(g_lua, l_object_field_info);
    lua_setfield(g_lua, -2, "ObjectFieldInfo");
    lua_pushcfunction(g_lua, l_object_get_field);
    lua_setfield(g_lua, -2, "ObjectGetField");
    lua_pushcfunction(g_lua, l_object_array_info);
    lua_setfield(g_lua, -2, "ObjectArrayInfo");
    lua_pushcfunction(g_lua, l_object_variant_index);
    lua_setfield(g_lua, -2, "ObjectVariantIndex");
    lua_pushcfunction(g_lua, l_object_map_key);
    lua_setfield(g_lua, -2, "ObjectMapKey");
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
    -- Floats print to full round-trip precision, and keep a decimal point
    -- even when whole, because that is what the real extender emits:
    -- Weight is 1.350000023841858 there and ValueScale is 1.0, where %.14g
    -- gave 1.3500000238419 and 1. A float and an integer are different
    -- types in Lua and the output should not blur them.
    if math.type(v) == "integer" then
      out[#out+1] = tostring(v)
    else
      local text = string.format("%.17g", v)
      -- %.17g is round-trip exact but verbose; prefer the shortest form
      -- that still reads back identically.
      for _, fmt in ipairs({"%.15g", "%.16g"}) do
        local short = string.format(fmt, v)
        if tonumber(short) == v then text = short break end
      end
      if not text:find("[.eE]") then text = text .. ".0" end
      out[#out+1] = text
    end
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
local make_array

-- Reads whatever is at a path, whichever kind it turns out to be.
--
-- One dispatch, because there were three: the component view, an array
-- element and a map value each had their own. They drifted, and the array one
-- asked the *container* for its element kind -- which reports "struct" for an
-- element that is itself a container, so reading a std::optional holding a
-- std::array raised instead of returning the array. Asking about the element's
-- own path instead is both correct and the same question in every case.
local read_path

read_path = function(handle, comp, path)
  local kind = Ext._Internal.FieldInfo(comp, path)
  if kind == nil then
    error("bg3le: " .. comp .. "." .. path .. " does not resolve", 0)
  end

  if kind == "array" then return make_array(handle, comp, path) end
  if kind == "map" then return make_map(handle, comp, path) end

  -- An optional holds nought or one. Empty reads as nil, which is the answer
  -- bg3se gives too, and stays distinct from unreadable, which raises. A full
  -- one reads as whatever it holds -- and what it holds may itself be a
  -- container, which is the case that was broken.
  if kind == "optional" then
    local held, err = Ext._Internal.ArrayInfo(handle, comp, path)
    if held == nil then
      error("bg3le: cannot size " .. comp .. "." .. path .. ": "
            .. tostring(err), 0)
    end
    if held == 0 then return nil end
    return read_path(handle, comp, path .. "[0]")
  end

  -- A variant reads as whatever it currently holds. Which alternative that is
  -- is a runtime fact, so it is asked for rather than derived -- and only the
  -- live one resolves, since the bytes are not any of the others.
  if kind == "variant" then
    local active, count = Ext._Internal.VariantIndex(handle, comp, path)
    if active == nil then
      error("bg3le: " .. comp .. "." .. path .. ": " .. tostring(count), 0)
    end
    if active >= count then return nil end  -- valueless
    return read_path(handle, comp, path .. "[" .. active .. "]")
  end

  if kind == "struct" then
    local inner, err = Ext._Internal.ComponentFields(comp, path)
    if inner == nil then error("bg3le: " .. tostring(err), 0) end
    return make_fields(handle, comp, path, inner)
  end

  local value, err = Ext._Internal.GetField(handle, comp, path)
  if value == nil and err ~= nil then error("bg3le: " .. err, 0) end
  return value
end

make_array = function(handle, comp, path)
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
    return read_path(handle, comp, element_path(i))
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
    return read_path(handle, comp, path .. "[" .. i .. "]")
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
        -- Any placeholder carries the slot number, because two keys that
        -- cannot be read still have to be distinct: identical ones collide
        -- into a single entry and the map reads as shorter than it is. A
        -- placeholder always arrives bracketed, which is also what keeps it
        -- from being mistaken for a real key.
        if k == nil then
          k = "<unreadable key " .. i .. ">"
        elseif type(k) == "string" and k:sub(1, 1) == "<" and k:sub(-1) == ">" then
          k = k:sub(1, -2) .. " at slot " .. i .. ">"
        end
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
      -- The field table says which kind it is, but read_path asks again
      -- about the field's own path. Both agree; going through the one
      -- dispatch is what keeps the three call sites from drifting.
      return read_path(handle, comp, path_to(key))
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

-- ---- Ext.Mod ----
--
-- Names from reference/ext-api-surface.txt; the shape of a mod from
-- reference/mod-shape.txt. GetLoadOrder returns an array of uuid strings, as
-- reference/mod-loadorder.txt shows.
Ext.Mod = {}

local function make_mod(addr)
  local info = Ext._Internal.ModInfo(addr)
  if info == nil then return nil end
  return {
    Info = info,
    Dependencies = Ext._Internal.ModList(addr, 0),
    ModConflicts = Ext._Internal.ModList(addr, 1),
    Addons = Ext._Internal.ModList(addr, 2),
  }
end

function Ext.Mod.GetLoadOrder()
  local out = {}
  local n = Ext._Internal.ModCount()
  for i = 0, n - 1 do
    local uuid = Ext._Internal.ModUuidAt(i)
    if uuid ~= nil then out[#out + 1] = uuid end
  end
  return out
end

function Ext.Mod.IsModLoaded(uuid)
  if type(uuid) ~= "string" then return false end
  return Ext._Internal.ModFind(uuid) ~= nil
end

function Ext.Mod.GetMod(uuid)
  if type(uuid) ~= "string" then return nil end
  local addr = Ext._Internal.ModFind(uuid)
  if addr == nil then return nil end
  return make_mod(addr)
end

-- ModManager::BaseModule, which is the campaign module rather than the first
-- mod in load order -- upstream returns that member, so this does too.
function Ext.Mod.GetBaseMod()
  local addr = Ext._Internal.ModBase()
  if addr == nil then return nil end
  return make_mod(addr)
end

-- Upstream returns the engine's ModManager. Settings is left out: it sits
-- past a hash map whose size on this build is not established, and an empty
-- table there would be a wrong answer rather than a missing one.
function Ext.Mod.GetModManager()
  local function collect(count, at)
    local out = {}
    for i = 0, count() - 1 do
      local addr = at(i)
      if addr ~= nil then out[#out + 1] = make_mod(addr) end
    end
    return out
  end

  return {
    BaseModule = Ext.Mod.GetBaseMod(),
    LoadOrderedModules = collect(Ext._Internal.ModCount,
                                 Ext._Internal.ModAt),
    AvailableMods = collect(Ext._Internal.ModAvailableCount,
                            Ext._Internal.ModAvailableAt),
  }
end

-- ---- Ext.Stats ----
--
-- A stat's attributes are not fields at fixed offsets; each one is a name, a
-- type and a raw integer, and the type decides how to read the integer. That
-- decoding lives here rather than in C so the shape of a stat is described in
-- one readable place.
--
-- RPGEnumerationType, in the order bg3se declares it. The C side returns
-- these numbers.
local STAT_KIND = {
  [0] = "Int", [1] = "Int64", [2] = "Float", [3] = "FixedString",
  [4] = "Enumeration", [5] = "Flags", [6] = "GUID", [7] = "StatsFunctors",
  [8] = "Conditions", [9] = "RollConditions", [10] = "Requirements",
  [11] = "MemorizationRequirements", [12] = "TranslatedString",
  [13] = "Unknown",
}

Ext.Stats = {}

-- One attribute, decoded as far as its type allows.
local function read_attribute(addr, i)
  local name, typeName, kind, raw = Ext._Internal.StatsAttrAt(addr, i)
  if name == nil then return nil end

  local value
  if kind == 0 or kind == 1 then
    value = raw
  elseif kind == 2 then
    value = Ext._Internal.StatsAttrFloat(raw) or {PoolIndex = raw}
  elseif kind == 6 then
    value = Ext._Internal.StatsAttrGuid(raw) or {PoolIndex = raw}
  elseif kind == 3 then
    -- Index 0 is the unset slot and does not resolve; an absent string is
    -- empty, not the number nought.
    value = Ext._Internal.StatsAttrString(raw) or ""
  elseif kind == 4 or kind == 5 then
    -- An enumeration matches one label exactly; a flag set indexes the
    -- int64 pool for a bitmask and may name several. Guessing at the latter
    -- produced a longsword proficient in clubs and light armour, so both
    -- paths now follow upstream's Object::GetFlags.
    if kind == 5 then
      -- A flag set is an array upstream, empty when nothing is set, so it is
      -- an array here. The public shape has to match or a mod that iterates
      -- it breaks.
      local joined = Ext._Internal.StatsAttrFlags(addr, i, raw)
      local list = {}
      if joined ~= nil and joined ~= "" then
        for part in joined:gmatch("[^;]+") do list[#list + 1] = part end
      end
      value = list
    else
      value = Ext._Internal.StatsAttrLabel(addr, i, raw) or raw
    end
  elseif kind == 10 then
    -- Requirements is an array upstream. Reading the entries needs
    -- Object::Requirements, which is not located yet, so the array is empty
    -- rather than absent: an empty list is the right shape and an honest
    -- value for a stat with no requirements, which most have.
    value = {}
  elseif kind == 7 then
    value = nil                      -- StatsFunctors: null upstream
  elseif kind == 8 or kind == 9 then
    -- Conditions resolve to their expression string upstream. The condition
    -- pool is not located yet, so an empty string keeps the type right.
    value = ""
  elseif kind == 12 then
    value = nil                      -- TranslatedString
  else
    value = raw
  end

  return name, value, STAT_KIND[kind] or "Unknown", typeName, raw
end

-- A stat object, shaped the way upstream shapes one.
--
-- Upstream returns a userdata proxy carrying bound methods and mod
-- provenance, not a plain table, and a mod written against it may call
-- stat:Sync() or read stat.ModId. Returning a table would make those fail
-- with "attempt to call a nil value", so this is a proxy with a metatable:
-- the attributes read through __index, the methods exist, and __pairs
-- enumerates both so a dump looks like upstream's.
--
-- The methods raise rather than pretend. Writing a stat needs
-- RPGStats::SyncWithPrototypeManager and the parse buffers behind it, none
-- of which bg3le reaches yet, and a silent no-op would be worse than an
-- error a mod author can read.
local STAT_NOT_WRITABLE =
  "bg3le cannot write stats yet; %s needs the engine's stat sync path, " ..
  "which is not implemented"

local function stat_method(name)
  return function() error(string.format(STAT_NOT_WRITABLE, name), 2) end
end

local STAT_METHODS = {
  Sync = stat_method("Sync"),
  SetPersistence = stat_method("SetPersistence"),
  SetRawAttribute = stat_method("SetRawAttribute"),
  CopyFrom = stat_method("CopyFrom"),
}

local stat_proxy = {}
stat_proxy.__index = function(self, key)
  local m = STAT_METHODS[key]
  if m ~= nil then return m end
  return rawget(self, "__fields")[key]
end

stat_proxy.__newindex = function(_, key, _)
  error(string.format(STAT_NOT_WRITABLE, "assigning " .. tostring(key)), 2)
end

-- Enumerates methods alongside fields, which is what makes a dump match:
-- upstream prints Sync, SetPersistence, SetRawAttribute and CopyFrom as
-- function entries next to the attributes.
stat_proxy.__pairs = function(self)
  local fields = rawget(self, "__fields")
  local keys = {}
  for k in pairs(fields) do keys[#keys + 1] = k end
  for k in pairs(STAT_METHODS) do keys[#keys + 1] = k end
  table.sort(keys)

  local i = 0
  return function()
    i = i + 1
    local k = keys[i]
    if k == nil then return nil end
    return k, STAT_METHODS[k] or fields[k]
  end
end

stat_proxy.__name = "Stat"

local function make_stat(fields)
  return setmetatable({__fields = fields}, stat_proxy)
end

-- Ext.Stats.Get(name) -> stat object, or nil plus a reason
function Ext.Stats.Get(name)
  if type(name) ~= "string" then
    return nil, "Ext.Stats.Get takes a stat name"
  end

  local addr, err = Ext._Internal.StatsFind(name)
  if addr == nil then return nil, err end

  local fields = {Name = name}
  local n = Ext._Internal.StatsAttrCount(addr)
  for i = 0, n - 1 do
    local attr, value = read_attribute(addr, i)
    if attr ~= nil then fields[attr] = value end
  end

  -- An empty attribute set means the discovery did not land, which is worth
  -- saying rather than returning a lone name that looks complete.
  if n == 0 then
    fields.AttributesUnavailable =
      "no attributes readable; see the stats lines in the extender log"
    return make_stat(fields)
  end

  -- Fields upstream puts alongside the attributes. Names and shapes follow
  -- reference/stats-weapon.txt rather than being chosen here.
  local modId, originalModId = Ext._Internal.StatOrigin(name)
  fields.ModId = modId
  fields.OriginalModId = originalModId
  fields.ModifierList = Ext._Internal.StatsType(addr)
  fields.ModifierListIndex = Ext._Internal.StatsListIndex(addr)
  fields.Using = Ext._Internal.StatsUsing(addr) or ""
  fields.ComboCategories = {}
  fields.ComboProperties = {}
  return make_stat(fields)
end

-- Ext.Stats.GetTypes(name) -> { attribute = type, ... }
--
-- Separate from Get because the values and their types are wanted for
-- different reasons, and putting both in one table would collide with the
-- attribute names.
function Ext.Stats.GetTypes(name)
  local addr, err = Ext._Internal.StatsFind(name)
  if addr == nil then return nil, err end
  local out = {}
  local n = Ext._Internal.StatsAttrCount(addr)
  for i = 0, n - 1 do
    local attr, _, kind, typeName = read_attribute(addr, i)
    if attr ~= nil then
      out[attr] = {Kind = kind, ValueList = typeName}
    end
  end
  return out
end

-- Ext.Stats.GetStats([modifierList]) -> { name, ... }
--
-- Upstream's name and signature. An earlier version called this GetAllStats
-- and added a GetStatsCount that upstream does not have; the public surface
-- has to match or a mod written against bg3se will not run here. Internal
-- entry points stay ours to shape -- it is Ext.* that is the contract.
--
-- No filter argument. Filtering by modifier list needs the modifier lists,
-- which are not located yet, and the obvious implementation -- StatsFind per
-- stat -- is quadratic: 15754 stats each costing a linear scan of 15754.
-- That would hang the story thread, which has already happened once on this
-- feature and is not worth repeating for a convenience.
function Ext.Stats.GetStats(modifierList)
  local out = {}
  local n = Ext._Internal.StatsCount()
  for i = 0, n - 1 do
    local name = Ext._Internal.StatsNameAt(i)
    if name == nil then goto continue end
    if modifierList == nil then
      out[#out + 1] = name
    else
      -- By index, never by name: StatsFind is a linear scan, so filtering
      -- through it would be 15754 scans of 15754 entries and would hang the
      -- story thread.
      local addr = Ext._Internal.StatsAt(i)
      if addr ~= nil and Ext._Internal.StatsType(addr) == modifierList then
        out[#out + 1] = name
      end
    end
    ::continue::
  end
  return out
end

-- Not part of upstream's surface, so it lives under _Internal where our own
-- additions belong; #Ext.Stats.GetStats() is the public way to count.
function Ext._Internal.StatsTotal() return Ext._Internal.StatsCount() end

-- ---- Ext.StaticData ----
--
-- Static data is read as a snapshot rather than as a live view, which is the
-- one place this differs from a component. A component can change under you,
-- so its fields are read on access; a resource definition is loaded once and
-- does not, so a plain table is simpler and more useful -- it can be held,
-- compared and serialised without reading the game again.
--
-- Fields go through the same metadata and the same resolver a component's do;
-- only the base address comes from elsewhere. A kind bg3le cannot convert
-- arrives as a marker rather than being dropped, for the reason it does
-- everywhere else.
local function read_object(addr, class, prefix, out)
  local fields, err = Ext._Internal.ObjectFields(class, prefix)
  if fields == nil then error("bg3le: " .. tostring(err), 0) end

  for name, kind in pairs(fields) do
    local path = (prefix == "") and name or (prefix .. "." .. name)
    if kind == "unsupported" then
      out[name] = "<unsupported>"
    elseif kind == "struct" then
      out[name] = read_object(addr, class, path, {})
    elseif kind == "array" then
      local count = Ext._Internal.ObjectArrayInfo(addr, class, path)
      local items = {}
      for i = 0, (count or 0) - 1 do
        local value, ferr = Ext._Internal.ObjectGetField(
          addr, class, path .. "[" .. i .. "]")
        if value == nil and ferr ~= nil then
          items[i + 1] = "<unreadable>"
        else
          items[i + 1] = value
        end
      end
      out[name] = items
    else
      local value, ferr = Ext._Internal.ObjectGetField(addr, class, path)
      if value == nil and ferr ~= nil then
        out[name] = "<unreadable>"
      else
        out[name] = value
      end
    end
  end
  return out
end

Ext.StaticData = {}

-- Ext.StaticData.Get(guid, type), where type is the resource class name such
-- as "ActionResource". Returns nil plus a reason, so a caller can tell "no
-- such GUID" from "no such resource type".
function Ext.StaticData.Get(guid, resourceType)
  if type(guid) ~= "string" or type(resourceType) ~= "string" then
    return nil, "Ext.StaticData.Get takes a GUID string and a type name"
  end

  local addr, err = Ext._Internal.ResourceGet(resourceType, guid)
  if addr == nil then return nil, err end
  return read_object(addr, resourceType, "", {})
end

-- Every GUID in a resource bank, so a type can be enumerated.
function Ext.StaticData.GetAll(resourceType)
  return Ext._Internal.ResourceGuids(resourceType)
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
