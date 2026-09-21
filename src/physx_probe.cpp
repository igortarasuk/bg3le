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
// physx::Sn::ConvX::convert(PxInputStream&, unsigned int, PxOutputStream&...)
constexpr std::uintptr_t kConvXConvert = 0x565fb70;
// physx::Sn::getBinaryPlatformName(unsigned int)
constexpr std::uintptr_t kGetPlatformName = 0x565f600;

using CheckProc = long (*)(const char*);
using ConvertProc = long (*)(void*, void*, void*, void*, void*, void*);
using PlatformNameProc = const char* (*)(unsigned);

CheckProc g_real_check = nullptr;
ConvertProc g_real_convert = nullptr;
PlatformNameProc g_platform_name = nullptr;

std::atomic<unsigned long> g_checks{0};
std::atomic<unsigned long> g_converts{0};
std::atomic<unsigned long> g_convert_ns{0};

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
    const double t0 = now_ns();
    const long rc = g_real_convert != nullptr ? g_real_convert(self, a, b, c, d, e) : 0;
    g_convert_ns.fetch_add((unsigned long)(now_ns() - t0), std::memory_order_relaxed);
    g_converts.fetch_add(1, std::memory_order_relaxed);
    return rc;
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
    if (hook_call_sites(kConvXConvert, reinterpret_cast<void*>(&convert_hook),
                        &original) > 0) {
        g_real_convert = reinterpret_cast<ConvertProc>(original);
    }

    // Report what this build considers native, for comparison with the tags.
    g_platform_name = reinterpret_cast<PlatformNameProc>(
        physx_resolve(kGetPlatformName));
    statusf("physx: probe installed");
}

void physx_probe_report(const char* when) {
    const unsigned long conv = g_converts.load();
    const unsigned long ns = g_convert_ns.load();
    statusf("physx (%s): %lu compatibility checks, %lu conversions, %.1fs total "
            "inside ConvX::convert",
            when, g_checks.load(), conv, (double)ns / 1e9);
    if (conv > 0) {
        statusf("physx (%s): mean %.3f ms per conversion", when,
                (double)ns / 1e6 / (double)conv);
    }
}

}  // namespace bg3le
