// Instrumenting PhysX's binary-format conversion, which is what makes the
// native build's level loads take 65-78s.
//
// Offsets are link-time addresses in bg3 4.8.400.7143220, from
// nm + tools/recover_symbols.py. Each hook validates its own call sites.

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "debug_server.h"
#include "hook.h"
#include "log.h"
#include "mem.h"
#include "physx_probe.h"

namespace bg3le {
namespace {

// physx::Sn::checkCompatibility(char const*)
constexpr std::uintptr_t kCheckCompatibility = 0x565f790;
// physx::Sn::ConvX::convert(PxInputStream&, unsigned, PxOutputStream&, ...)
// has no direct callers: ConvX is polymorphic and PxBinaryConverter reaches
// it through the vtable, so hook the slot instead.
constexpr std::uintptr_t kConvXConvert = 0x565fb70;
constexpr std::uintptr_t kConvXConvertSlot = 0x78a6748;
// physx::Sn::ConvX::convertClass(char const*, MetaClass const*, int) -- 14
// direct call sites, and the per-class unit of work.
constexpr std::uintptr_t kConvertClass = 0x5660120;
// physx::Sn::getBinaryPlatformName(unsigned int)
constexpr std::uintptr_t kGetPlatformName = 0x565f600;
// physx::shdfnd::TempAllocator::allocate / ::deallocate -- both take a
// single global MutexImpl, which is the suspected bottleneck.
constexpr std::uintptr_t kTempAlloc = 0x5666b30;
constexpr std::uintptr_t kTempFree = 0x5666c90;

using CheckProc = long (*)(const char*);
using ConvertProc = long (*)(void*, void*, void*, void*, void*, void*);
using ConvertClassProc = long (*)(void*, const char*, const void*, int);
using TempAllocProc = void* (*)(unsigned long, const char*, int);
using TempFreeProc = void (*)(void*);
using PlatformNameProc = const char* (*)(unsigned);

CheckProc g_real_check = nullptr;
ConvertProc g_real_convert = nullptr;
ConvertClassProc g_real_convert_class = nullptr;
TempAllocProc g_real_temp_alloc = nullptr;
TempFreeProc g_real_temp_free = nullptr;
PlatformNameProc g_platform_name = nullptr;

std::atomic<unsigned long> g_checks{0};
std::atomic<unsigned long> g_converts{0};
std::atomic<unsigned long> g_convert_ns{0};
std::atomic<unsigned long> g_classes{0};
std::atomic<unsigned long> g_class_ns{0};
std::atomic<unsigned long> g_class_outer{0};
std::atomic<unsigned long> g_allocs{0};
std::atomic<unsigned long> g_alloc_ns{0};
std::atomic<unsigned long> g_frees{0};
std::atomic<unsigned long> g_free_ns{0};

double now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// The argument is the platform tag baked into the serialized blob.
long check_hook(const char* tag) {
    const unsigned long n = g_checks.fetch_add(1, std::memory_order_relaxed);
    if (n < 4) {
        char buf[64];
        statusf("physx: checkCompatibility(\"%s\")",
                safe_cstr(tag, buf, sizeof(buf)) ? buf : "<unreadable>");
    }
    return g_real_check != nullptr ? g_real_check(tag) : 0;
}

// Timing each conversion separates "a lot of work" from "a little work
// serialised on PhysX's shared allocator" -- which decides whether caching
// the results would help at all.
long convert_hook(void* self, void* a, void* b, void* c, void* d, void* e) {
    thread_local int nesting = 0;
    g_converts.fetch_add(1, std::memory_order_relaxed);

    const bool outermost = (nesting == 0);
    ++nesting;
    const double t0 = outermost ? now_ns() : 0.0;
    const long rc = g_real_convert != nullptr ? g_real_convert(self, a, b, c, d, e) : 0;
    --nesting;
    if (outermost) {
        g_convert_ns.fetch_add((unsigned long)(now_ns() - t0),
                               std::memory_order_relaxed);
    }
    return rc;
}

// The per-class unit of conversion work. Timing it separately from the
// top-level convert shows how much of the wall time is spent inside the
// conversion itself versus waiting to get into it.
long convert_class_hook(void* self, const char* name, const void* meta, int depth) {
    // convertClass recurses, so timing every invocation counts the same
    // interval once per nesting level. Only time the outermost call per
    // thread; count them all.
    thread_local int nesting = 0;
    g_classes.fetch_add(1, std::memory_order_relaxed);

    const bool outermost = (nesting == 0);
    ++nesting;
    const double t0 = outermost ? now_ns() : 0.0;
    const long rc = g_real_convert_class != nullptr
                        ? g_real_convert_class(self, name, meta, depth)
                        : 0;
    --nesting;
    if (outermost) {
        g_class_ns.fetch_add((unsigned long)(now_ns() - t0),
                             std::memory_order_relaxed);
        g_class_outer.fetch_add(1, std::memory_order_relaxed);
    }
    return rc;
}

// If most of the conversion time is in here, the fix is the allocator, not
// the asset format.
void* temp_alloc_hook(unsigned long size, const char* file, int line) {
    const double t0 = now_ns();
    void* p = g_real_temp_alloc != nullptr ? g_real_temp_alloc(size, file, line)
                                           : nullptr;
    g_alloc_ns.fetch_add((unsigned long)(now_ns() - t0), std::memory_order_relaxed);
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    return p;
}

void temp_free_hook(void* p) {
    const double t0 = now_ns();
    if (g_real_temp_free != nullptr) g_real_temp_free(p);
    g_free_ns.fetch_add((unsigned long)(now_ns() - t0), std::memory_order_relaxed);
    g_frees.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

void physx_probe_install() {
    const char* opt = std::getenv("BG3LE_PHYSX_PROBE");
    if (opt == nullptr || opt[0] != '1') return;

    void* original = nullptr;
    if (hook_call_sites(kCheckCompatibility, reinterpret_cast<void*>(&check_hook),
                        &original) > 0) {
        g_real_check = reinterpret_cast<CheckProc>(original);
    }
    if (hook_slot(kConvXConvertSlot, kConvXConvert,
                  reinterpret_cast<void*>(&convert_hook), &original)) {
        g_real_convert = reinterpret_cast<ConvertProc>(original);
    }
    if (hook_call_sites(kConvertClass, reinterpret_cast<void*>(&convert_class_hook),
                        &original) > 0) {
        g_real_convert_class = reinterpret_cast<ConvertClassProc>(original);
    }

    if (hook_call_sites(kTempAlloc, reinterpret_cast<void*>(&temp_alloc_hook),
                        &original) > 0) {
        g_real_temp_alloc = reinterpret_cast<TempAllocProc>(original);
    }
    if (hook_call_sites(kTempFree, reinterpret_cast<void*>(&temp_free_hook),
                        &original) > 0) {
        g_real_temp_free = reinterpret_cast<TempFreeProc>(original);
    }

    // Report what this build considers native, for comparison with the tags.
    g_platform_name = reinterpret_cast<PlatformNameProc>(
        physx_resolve(kGetPlatformName));
    statusf("physx: probe installed");
}

void physx_probe_reset() {
    g_checks.store(0);
    g_converts.store(0);
    g_convert_ns.store(0);
    g_classes.store(0);
    g_class_ns.store(0);
    g_class_outer.store(0);
    g_allocs.store(0);
    g_alloc_ns.store(0);
    g_frees.store(0);
    g_free_ns.store(0);
}

void physx_probe_report(const char* when) {
    const unsigned long conv = g_converts.load();
    const unsigned long ns = g_convert_ns.load();
    const unsigned long cls = g_classes.load();
    const unsigned long cls_ns = g_class_ns.load();
    const unsigned long outer = g_class_outer.load();
    // Times are summed across worker threads, so they legitimately exceed
    // wall time; nested calls are excluded so they are not double-counted.
    statusf("physx (%s): %lu version checks, %lu conversions, "
            "%lu convertClass calls (%lu outermost)",
            when, g_checks.load(), conv, cls, outer);
    statusf("physx (%s): %.2fs thread-time in conversions, %.2fs in outermost "
            "convertClass", when, (double)ns / 1e9, (double)cls_ns / 1e9);

    const unsigned long an = g_alloc_ns.load();
    const unsigned long fn = g_free_ns.load();
    statusf("physx (%s): TempAllocator %lu allocs (%.2fs) + %lu frees (%.2fs) "
            "= %.1f%% of convertClass time",
            when, g_allocs.load(), (double)an / 1e9, g_frees.load(),
            (double)fn / 1e9,
            cls_ns > 0 ? 100.0 * (double)(an + fn) / (double)cls_ns : 0.0);
}

}  // namespace bg3le
