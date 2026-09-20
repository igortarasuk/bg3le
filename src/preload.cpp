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
#include "lua_host.h"
#include "mem.h"
#include "log.h"

namespace bg3le {
namespace {

SymbolTable g_symbols;
std::once_flag g_symbols_once;
std::once_flag g_story_once;
std::once_flag g_init_struct_once;

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
        lua_init();
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

// TOsirisInitFunction is the callback table the game hands to Osiris. It
// should carry the dispatch entry used to invoke DIV functions, which is the
// call path Osi.* needs. Classify each word to find out what's in it.
void dump_init_struct(const void* init_fn) {
    std::uintptr_t words[16] = {};
    if (!safe_read(init_fn, words, sizeof(words))) {
        logf("init struct: unreadable at %p", init_fn);
        return;
    }

    logf("TOsirisInitFunction @ %p", init_fn);
    for (unsigned i = 0; i < 16; ++i) {
        const std::uintptr_t w = words[i];
        const std::size_t off = i * sizeof(void*);

        if (w == 0) {
            logf("  +%02zu = 0", off);
            continue;
        }

        Dl_info info{};
        if (::dladdr(reinterpret_cast<void*>(w), &info) != 0 && info.dli_fname != nullptr) {
            const char* base = std::strrchr(info.dli_fname, '/');
            logf("  +%02zu = 0x%016lx  %s+0x%lx  %s", off, (unsigned long)w,
                 base != nullptr ? base + 1 : info.dli_fname,
                 (unsigned long)(w - reinterpret_cast<std::uintptr_t>(info.dli_fbase)),
                 info.dli_sname != nullptr ? info.dli_sname : "");
            continue;
        }

        char text[64];
        if (safe_cstr(reinterpret_cast<const void*>(w), text, sizeof(text)) &&
            text[0] >= 0x20 && text[0] < 0x7f) {
            logf("  +%02zu = 0x%016lx  \"%s\"", off, (unsigned long)w, text);
        } else {
            logf("  +%02zu = 0x%016lx", off, (unsigned long)w);
        }
    }
}

// The table's two large entries (+08, +16) are the likely Call/Query
// handlers Osiris invokes for DIV functions; the rest are small thunks.
// Rather than guess, hand Osiris a copy with those two wrapped and learn the
// signature from real traffic.
//
// Wrappers take six longs so they forward correctly whatever the true arity
// is: SysV passes the first six integer args in registers, and the callee
// reads only what it needs. Opt in with BG3LE_WRAP_DIV=1.
using Thunk6 = long (*)(long, long, long, long, long, long);

Thunk6 g_real_call = nullptr;
Thunk6 g_real_query = nullptr;

// Cross-check the raw bytes of a live COsiArgumentDesc against Osiris' own
// exported accessors, so the layout is read off the engine rather than guessed.
void test_requery(unsigned id, void* args);

void dump_arg_desc(const void* desc, unsigned id, const char* kind) {
    auto type_of = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc13GetOpaqueTypeEv");
    auto is_str = next<bool (*)(const void*)>("_ZNK16COsiArgumentDesc12IsStringTypeEv");
    auto get_int = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc10GetIntegerEv");
    auto get_str = next<const char* (*)(const void*)>(
        "_ZNK16COsiArgumentDesc12GetAnyStringEv");
    auto src_ptr = next<const void* (*)(const void*, bool)>(
        "_ZNK16COsiArgumentDesc13GetDataSrcPtrEb");
    auto data_size = next<unsigned long (*)(const void*, bool)>(
        "_ZNK16COsiArgumentDesc11GetDataSizeEb");

    logf("%s id=0x%08x chain from %p", kind, id, desc);

    const void* node = desc;
    for (unsigned n = 0; n < 8 && node != nullptr; ++n) {
        std::uintptr_t w[12] = {};
        if (!safe_read(node, w, sizeof(w))) {
            logf("  node[%u] @ %p unreadable", n, node);
            break;
        }

        const int type = type_of != nullptr ? type_of(node) : -1;
        const bool str = is_str != nullptr && is_str(node);
        logf("  node[%u] @ %p type=%d isString=%d", n, node, type, (int)str);
        for (unsigned i = 0; i < 12; ++i)
            logf("      +%02zu = 0x%016lx", i * sizeof(void*), (unsigned long)w[i]);

        // Where does the value actually live? This is what constructing one needs.
        if (src_ptr != nullptr) logf("      GetDataSrcPtr(false) = %p", src_ptr(node, false));
        auto dst_ptr = next<void* (*)(void*)>("_ZN16COsiArgumentDesc13GetDataDstPtrEv");
        if (dst_ptr != nullptr)
            logf("      GetDataDstPtr()      = %p", dst_ptr(const_cast<void*>(node)));
        if (data_size != nullptr) logf("      GetDataSize(false)   = %lu", data_size(node, false));

        if (str && get_str != nullptr) {
            char buf[128];
            const char* v = get_str(node);
            logf("      GetAnyString() = \"%s\"",
                 safe_cstr(v, buf, sizeof(buf)) ? buf : "<unreadable>");
        } else if (!str && get_int != nullptr) {
            logf("      GetInteger()   = %d", get_int(node));
        }

        node = reinterpret_cast<const void*>(w[0]);  // suspected NextParam
    }
}

long call_wrapper(long a, long b, long c, long d, long e, long f) {
    static unsigned long seen = 0;
    if (++seen <= 10) logf("DIV Call  arg0=0x%lx arg1=0x%lx", a, b);
    if (seen <= 3) dump_arg_desc(reinterpret_cast<const void*>(b),
                                 (unsigned)a, "DIV Call ");
    return g_real_call != nullptr ? g_real_call(a, b, c, d, e, f) : 0;
}

long query_wrapper(long a, long b, long c, long d, long e, long f) {
    static unsigned long seen = 0;
    if (++seen <= 10) logf("DIV Query arg0=0x%lx arg1=0x%lx", a, b);
    if (seen <= 3) dump_arg_desc(reinterpret_cast<const void*>(b),
                                 (unsigned)a, "DIV Query");
    static std::once_flag once;
    if ((unsigned)a == 0x8000113au || (unsigned)a == 0x800019f2u)
        std::call_once(once, [a, b] {
            test_requery((unsigned)a, reinterpret_cast<void*>(b));
        });
    return g_real_query != nullptr ? g_real_query(a, b, c, d, e, f) : 0;
}

// Returns the table to pass on: either our patched copy or the original.
void* maybe_wrap_div_table(void* init_fn) {
    static std::uintptr_t copy[32];
    if (!safe_read(init_fn, copy, sizeof(copy))) {
        logf("DIV table unreadable, passing through");
        return init_fn;
    }

    // Always record the handlers; interposing them is a separate opt-in.
    g_real_call = reinterpret_cast<Thunk6>(copy[1]);
    g_real_query = reinterpret_cast<Thunk6>(copy[2]);

    const char* opt = std::getenv("BG3LE_WRAP_DIV");
    if (opt == nullptr || opt[0] != '1') return init_fn;

    copy[1] = reinterpret_cast<std::uintptr_t>(&call_wrapper);
    copy[2] = reinterpret_cast<std::uintptr_t>(&query_wrapper);
    logf("DIV wrap: active (call=%p query=%p)", (void*)g_real_call, (void*)g_real_query);
    return copy;
}

double now_s() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// First attempt at invoking Osiris ourselves. IntegerSum is pure arithmetic
// with a checkable answer, so a wrong layout shows up as a bad result rather
// than as damage to a save. Opt in with BG3LE_TEST_CALL=1.
// Building a descriptor from zeroed memory crashes inside SetInteger, so
// prove invocation a different way: re-issue a query the engine just made,
// reusing its own descriptor list. Exists/IsSummon are pure predicates, so
// calling one twice is harmless, and nothing has to be constructed.
void test_requery(unsigned id, void* args) {
    const char* opt = std::getenv("BG3LE_TEST_CALL");
    if (opt == nullptr || opt[0] != '1') return;
    if (g_real_query == nullptr) return;

    // Exists(GUIDSTRING, INTEGER) and IsSummon(GUIDSTRING, INTEGER).
    if (id != 0x8000113au && id != 0x800019f2u) return;

    auto get_int = next<int (*)(const void*)>("_ZNK16COsiArgumentDesc10GetIntegerEv");
    std::uintptr_t next_node = 0;
    if (!safe_read(args, &next_node, sizeof(next_node)) || next_node == 0) return;
    const void* out = reinterpret_cast<const void*>(next_node);

    const int before = get_int != nullptr ? get_int(out) : -1;
    logf("requery: id=0x%08x out before = %d; re-invoking with engine's own args ...",
         id, before);
    long rc = g_real_query(static_cast<long>(id), reinterpret_cast<long>(args),
                           0, 0, 0, 0);
    logf("requery: rc=%ld out after = %d", rc, get_int != nullptr ? get_int(out) : -1);
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
    std::call_once(g_init_struct_once, [init_fn] { dump_init_struct(init_fn); });
    void* table = maybe_wrap_div_table(init_fn);
    return real != nullptr ? real(self, table) : 0;
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
