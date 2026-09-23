#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <vector>
#include <unistd.h>

namespace bg3le {
namespace {
std::FILE* g_log = nullptr;
std::mutex g_mutex;
char g_path[4096] = {0};
}

const char* log_path() { return g_path; }

namespace {

// Escape sequences belong on a terminal, not in a log file. Mods colour
// their own output -- Mod Configuration Menu writes 24-bit SGR codes -- and
// a log full of them is hard to read and worse to quote: paste it anywhere
// that drops the escape byte and the digits stay behind as text, which is
// where "[38;2;0;255;255;48;2;12;12;12m" in a bug report comes from.
void write_without_escapes(std::FILE* f, char const* text,
                           std::size_t length) {
    std::size_t at = 0;
    while (at < length) {
        if (text[at] != '\x1b') {
            std::fputc(text[at++], f);
            continue;
        }

        // CSI: ESC [ parameters intermediates final. Anything else escape-led
        // is two bytes, and an unterminated one ends the line.
        ++at;
        if (at < length && text[at] == '[') {
            ++at;
            while (at < length && (unsigned char)text[at] >= 0x20
                   && (unsigned char)text[at] <= 0x3f) {
                ++at;
            }
            if (at < length) ++at;  // the final byte
        } else if (at < length) {
            ++at;
        }
    }
}

}  // namespace

void log_init() {
    // Every process in the Steam runtime launch chain preloads us, so each
    // needs its own file or they truncate each other.
    const char* base = std::getenv("BG3LE_LOG");
    std::snprintf(g_path, sizeof(g_path), "%s.%d",
                  base != nullptr ? base : "/tmp/bg3le.log", (int)::getpid());
    g_log = std::fopen(g_path, "w");
}

void logf(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_log == nullptr) return;

    // Formatted first so the escapes can be taken out. A line longer than
    // the buffer -- a component dump, say -- gets one sized for it rather
    // than being truncated.
    char stack[2048];
    char* text = stack;
    std::vector<char> heap;

    va_list ap;
    va_start(ap, fmt);
    va_list measure;
    va_copy(measure, ap);
    int length = std::vsnprintf(stack, sizeof(stack), fmt, ap);
    va_end(ap);

    if (length >= (int)sizeof(stack)) {
        heap.resize((std::size_t)length + 1);
        std::vsnprintf(heap.data(), heap.size(), fmt, measure);
        text = heap.data();
    }
    va_end(measure);
    if (length < 0) return;

    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    std::fprintf(g_log, "[%6ld.%03ld] ", ts.tv_sec, ts.tv_nsec / 1000000);
    write_without_escapes(g_log, text, (std::size_t)length);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}
}  // namespace bg3le
