// The logging bg3le's memory steering expects, for the standalone build.
//
// src/vulkan_memory.cpp is shared: bg3le links it against the extender's own
// log, and memsteer.so links it against this, which writes to stderr. One
// implementation, so the tool and the extender cannot drift --
// the whole point of extracting it was to apply the same steering to other
// native Vulkan games, not to maintain a second copy of it.
//
// MEMSTEER_LOG names a file to write to instead of stderr, since a game
// launched through Steam generally has nowhere useful for stderr to go.

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "log.h"

namespace {

std::FILE* sink() {
    static std::FILE* f = [] () -> std::FILE* {
        const char* path = std::getenv("MEMSTEER_LOG");
        if (path == nullptr || path[0] == '\0') return stderr;
        std::FILE* opened = std::fopen(path, "we");
        return opened != nullptr ? opened : stderr;
    }();
    return f;
}

std::mutex& writing() {
    static std::mutex m;
    return m;
}

}  // namespace

namespace bg3le {

void log_init() {}

const char* log_path() {
    const char* path = std::getenv("MEMSTEER_LOG");
    return path != nullptr ? path : "";
}

void logf(const char* fmt, ...) {
    const std::lock_guard<std::mutex> held(writing());

    // No prefix of its own: the messages already carry "vkmem: ", and the
    // text should read the same whether it came from the extender's log or
    // from this tool's.
    std::FILE* out = sink();

    std::va_list args;
    va_start(args, fmt);
    std::vfprintf(out, fmt, args);
    va_end(args);

    std::fputc('\n', out);
    std::fflush(out);
}

}  // namespace bg3le
