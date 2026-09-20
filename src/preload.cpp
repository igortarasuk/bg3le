// bg3le - Linux script extender shim for the native Baldur's Gate 3 build.
//
// Loaded via LD_PRELOAD.  Osiris entry points are plain PLT calls from the
// bg3 executable into libOsiris.so, so they are interposed by defining the
// same mangled symbols here and chaining with dlsym(RTLD_NEXT, ...).

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

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

// MappingInfo's layout is unknown; dump candidate entries so it can be
// derived from what the engine actually hands back.
void probe_mappings(void* self, const char* getter, const char* what) {
    auto fn = next<long (*)(void*, void**, unsigned*)>(getter);
    if (fn == nullptr) {
        logf("%s: getter unresolved", what);
        return;
    }

    void* infos = nullptr;
    unsigned count = 0;
    fn(self, &infos, &count);
    logf("%s: count=%u base=%p", what, count, infos);
    if (infos == nullptr || count == 0) return;

    // Print the first few machine words of the first entries, resolving any
    // word that looks like a char* so the name field reveals itself.
    for (unsigned e = 0; e < (count < 3u ? count : 3u); ++e) {
        std::uintptr_t words[8] = {};
        const char* entry = static_cast<const char*>(infos) + e * sizeof(words);
        if (!safe_read(entry, words, sizeof(words))) {
            logf("  [%u] unreadable", e);
            continue;
        }
        for (unsigned w = 0; w < 8; ++w) {
            char text[96];
            if (safe_cstr(reinterpret_cast<const void*>(words[w]), text, sizeof(text)) &&
                text[0] >= 0x20 && text[0] < 0x7f) {
                logf("  [%u] +%02zu = 0x%016lx -> \"%s\"", e, w * sizeof(void*),
                     (unsigned long)words[w], text);
            } else {
                logf("  [%u] +%02zu = 0x%016lx", e, w * sizeof(void*),
                     (unsigned long)words[w]);
            }
        }
    }
}

}  // namespace
}  // namespace bg3le

using namespace bg3le;

// ---- Osiris interposition ----

extern "C" long _ZN7COsiris8InitGameEv(void* self) {
    static auto real = next<long (*)(void*)>("_ZN7COsiris8InitGameEv");
    logf("COsiris::InitGame() self=%p", self);
    long rc = real != nullptr ? real(self) : 0;

    // The story is loaded by now, so the mapping tables are populated.
    ensure_symbols();
    probe_mappings(self, "_ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj",
                   "function mappings");
    probe_mappings(self, "_ZN7COsiris15GetTypeMappingsEPP11MappingInfoPj",
                   "type mappings");
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

    // First event means the story is running, so the tables exist by now.
    std::call_once(g_story_once, [self] {
        ensure_symbols();

        auto gen = next<long (*)(void*)>("_ZN7COsiris20GenerateFunctionListEv");
        if (gen != nullptr) {
            logf("GenerateFunctionList() ...");
            gen(self);
            logf("GenerateFunctionList() returned");
        } else {
            logf("GenerateFunctionList unresolved");
        }

        probe_mappings(self, "_ZN7COsiris19GetFunctionMappingsEPP11MappingInfoPj",
                       "function mappings");
        probe_mappings(self, "_ZN7COsiris15GetTypeMappingsEPP11MappingInfoPj",
                       "type mappings");
        probe_mappings(self, "_ZN7COsiris17GetObjectMappingsEPP11MappingInfoPj",
                       "object mappings");
    });

    static unsigned long seen = 0;
    if (++seen <= 5) logf("COsiris::Event(%u) args=%p", event_id, args);
    return real != nullptr ? real(self, event_id, args) : 0;
}

// ---- entry point ----

__attribute__((constructor)) static void bg3le_init() {
    log_init();
    logf("bg3le loaded");

    // Symbol loading is deferred to the first Osiris callback: allocating
    // here runs before the game's allocator exists.
    logf("waiting for game init");
}
