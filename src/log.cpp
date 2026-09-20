#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <unistd.h>

namespace bg3le {
namespace {
std::FILE* g_log = nullptr;
std::mutex g_mutex;
}

void log_init() {
    // Every process in the Steam runtime launch chain preloads us, so each
    // needs its own file or they truncate each other.
    const char* base = std::getenv("BG3LE_LOG");
    char path[4096];
    std::snprintf(path, sizeof(path), "%s.%d",
                  base != nullptr ? base : "/tmp/bg3le.log", (int)::getpid());
    g_log = std::fopen(path, "w");
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log == nullptr) return;
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::fprintf(g_log, "[%6ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(g_log, fmt, ap);
    va_end(ap);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}
}  // namespace bg3le
