// bg3le - Linux script extender shim for the native Baldur's Gate 3 build.
//
// Loaded via LD_PRELOAD.  Osiris entry points are plain PLT calls from the
// bg3 executable into libOsiris.so, so they are interposed by defining the
// same mangled symbols here and chaining with dlsym(RTLD_NEXT, ...).

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>
#include <ctime>
#include <unistd.h>

#include "elf_symbols.h"
#include "mem.h"
#include "log.h"

namespace bg3le {
namespace {

SymbolTable g_symbols;
std::once_flag g_symbols_once;
std::once_flag g_story_once;

void ensure_symbols();

template <typename Fn>
Fn next(const char* mangled) {
    return reinterpret_cast<Fn>(::dlsym(RTLD_NEXT, mangled));
}

void ensure_symbols() {
    std::call_once(g_symbols_once, [] {
        if (!g_symbols.load()) {
            logf("symbol table unavailable");
            return;
        }
        logf("symbols: %zu from %s (bias 0x%lx)", g_symbols.count(),
             g_symbols.path().c_str(), g_symbols.bias());
        void* p = g_symbols.find(
            "_ZN2ls11TypeContextIN3esv4tags8_private23TagComponentTypeContextEE7m_StateE");
        logf("  sentinel esv TagComponentTypeContext::m_State -> %p", p);
    });
}

// Derived from the engine's own tables: a flat 24-byte record. Function ids
// always carry 0x80000000; param_types points into one contiguous byte array
// shared by all entries.
struct MappingInfo {
    const char* name;
    std::uint32_t id;
    std::uint32_t num_params;
    const std::uint8_t* param_types;
};
static_assert(sizeof(MappingInfo) == 24, "unexpected MappingInfo layout");

using GetMappings = long (*)(void*, MappingInfo**, unsigned*);
using FreeMappings = long (*)(void*, MappingInfo*, unsigned);

// Ids 0-5 are Osiris' built-in value types and are absent from the story's
// type table; 6+ are the story enums (CHARACTER, ITEM, FLAG, ...).
const char* base_type_name(std::uint8_t t) {
    switch (t) {
        case 0: return "NONE";
        case 1: return "INTEGER";
        case 2: return "INTEGER64";
        case 3: return "REAL";
        case 4: return "STRING";
        case 5: return "GUIDSTRING";
        default: return nullptr;
    }
}

std::string name_of(const MappingInfo& m) {
    char buf[256];
    return safe_cstr(m.name, buf, sizeof(buf)) ? std::string(buf) : std::string("?");
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Whichever of InitGame / the first Event happens first does the work.
void dump_osiris_api(void* self);

void dump_once(void* self) {
    std::call_once(g_story_once, [self] {
        ensure_symbols();
        dump_osiris_api(self);
    });
}

void dump_osiris_api(void* self) {
    auto gen = next<long (*)(void*)>("_ZN7COsiris20GenerateFunctionListEv");
    if (gen != nullptr) gen(self);  // mappings are generated on demand

    auto get_types = next<GetMappings>("_ZN7COsiris15GetTypeMappingsEPP11MappingInfoPj");
    auto get_funcs = next<GetMappings>("_ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj");
    if (get_types == nullptr || get_funcs == nullptr) {
        logf("osiris: mapping getters unresolved");
        return;
    }

    MappingInfo* types = nullptr;
    unsigned type_count = 0;
    get_types(self, &types, &type_count);

    std::unordered_map<std::uint32_t, std::string> type_names;
    for (unsigned i = 0; i < type_count; ++i) type_names[types[i].id] = name_of(types[i]);

    MappingInfo* funcs = nullptr;
    unsigned func_count = 0;
    get_funcs(self, &funcs, &func_count);
    logf("osiris: %u functions, %u types", func_count, type_count);

    char path[4096];
    const char* out = std::getenv("BG3LE_OSI_DUMP");
    if (out != nullptr) {
        std::snprintf(path, sizeof(path), "%s", out);
    } else {
        std::snprintf(path, sizeof(path), "/tmp/bg3le-osi.%d.txt", (int)::getpid());
    }
    std::FILE* f = std::fopen(path, "w");
    if (f != nullptr) {
        for (const auto& t : type_names) std::fprintf(f, "type %u %s\n", t.first, t.second.c_str());
        for (unsigned i = 0; i < func_count; ++i) {
            const MappingInfo& m = funcs[i];
            std::fprintf(f, "func 0x%08x %s(", m.id, name_of(m).c_str());
            for (unsigned p = 0; p < m.num_params; ++p) {
                std::uint8_t t = 0;
                safe_read(m.param_types + p, &t, 1);
                std::fprintf(f, "%s", p ? ", " : "");
                auto it = type_names.find(t);
                if (it != type_names.end()) {
                    std::fprintf(f, "%s", it->second.c_str());
                } else if (const char* b = base_type_name(t)) {
                    std::fprintf(f, "%s", b);
                } else {
                    std::fprintf(f, "?%u", t);
                }
            }
            std::fprintf(f, ")\n");
        }
        std::fclose(f);
        logf("osiris: wrote %s", path);
    }

    auto free_types = next<FreeMappings>("_ZN7COsiris16FreeTypeMappingsEP11MappingInfoj");
    auto free_funcs = next<FreeMappings>("_ZN7COsiris20FreeFunctionMappingsEP11MappingInfoj");
    if (free_types != nullptr) free_types(self, types, type_count);
    if (free_funcs != nullptr) free_funcs(self, funcs, func_count);
}

}  // namespace
}  // namespace bg3le

using namespace bg3le;

// ---- Osiris interposition ----

extern "C" long _ZN7COsiris8InitGameEv(void* self) {
    static auto real = next<long (*)(void*)>("_ZN7COsiris8InitGameEv");
    logf("COsiris::InitGame() self=%p", self);
    long rc = real != nullptr ? real(self) : 0;
    dump_once(self);
    return rc;
}

extern "C" long _ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction(
    void* self, void* init_fn) {
    static auto real = next<long (*)(void*, void*)>(
        "_ZN7COsiris20RegisterDIVFunctionsEP19TOsirisInitFunction");
    logf("COsiris::RegisterDIVFunctions() self=%p init=%p", self, init_fn);
    ensure_symbols();  // game is initialised by now; its allocator is usable
    return real != nullptr ? real(self, init_fn) : 0;
}

extern "C" long _ZN7COsiris5EventEjP16COsiArgumentDesc(
    void* self, unsigned event_id, void* args) {
    static auto real = next<long (*)(void*, unsigned, void*)>(
        "_ZN7COsiris5EventEjP16COsiArgumentDesc");

    dump_once(self);  // first event means the story is up

    static unsigned long seen = 0;
    if (++seen <= 5) logf("COsiris::Event(%u) args=%p", event_id, args);
    return real != nullptr ? real(self, event_id, args) : 0;
}

// ---- story load timing ----
//
// A 72s stall sits between story registration and the first Osiris event on
// the native build but not under Proton. These four are the story-loading
// entry points, so timing them localises it.

extern "C" long _ZN7COsiris4LoadER12COsiSmartBuf(void* self, void* buf) {
    static auto real = next<long (*)(void*, void*)>("_ZN7COsiris4LoadER12COsiSmartBuf");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, buf) : 0;
    logf("COsiris::Load took %.2fs", now_s() - t0);
    return rc;
}

extern "C" long _ZN7COsiris7CompileEPKwS1_(void* self, const wchar_t* a, const wchar_t* b) {
    static auto real = next<long (*)(void*, const wchar_t*, const wchar_t*)>(
        "_ZN7COsiris7CompileEPKwS1_");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, a, b) : 0;
    logf("COsiris::Compile took %.2fs", now_s() - t0);
    return rc;
}

extern "C" long _ZN7COsiris5MergeEPKw(void* self, const wchar_t* a) {
    static auto real = next<long (*)(void*, const wchar_t*)>("_ZN7COsiris5MergeEPKw");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, a) : 0;
    logf("COsiris::Merge took %.2fs", now_s() - t0);
    return rc;
}

extern "C" long _ZN7COsiris12PrepareMergeEPKw(void* self, const wchar_t* a) {
    static auto real = next<long (*)(void*, const wchar_t*)>("_ZN7COsiris12PrepareMergeEPKw");
    double t0 = now_s();
    long rc = real != nullptr ? real(self, a) : 0;
    logf("COsiris::PrepareMerge took %.2fs", now_s() - t0);
    return rc;
}

// ---- entry point ----

__attribute__((constructor)) static void bg3le_init() {
    log_init();
    logf("bg3le loaded");

    // Symbol loading is deferred to the first Osiris callback: allocating
    // here runs before the game's allocator exists.
    logf("waiting for game init");
}
