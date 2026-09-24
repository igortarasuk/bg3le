#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "mem.h"

#include "log.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace bg3le {
namespace {
// Reading our own memory through process_vm_readv turns a bad pointer into
// EFAULT rather than SIGSEGV, which matters when probing engine structs.
// How many of these have been made, reported as they mount up.
//
// Every read here is a system call, which is the price of turning a bad
// pointer into EFAULT rather than a crash. That is cheap until something
// does it in a loop over a whole engine structure, and then it is the
// only thing that matters -- so the number can be looked at rather than
// inferred. BG3LE_COUNT_READS=1 turns it on.
//
// It is how the last of the stats work was found: a mod's pass was making
// two hundred and twenty million of these, a million a second, and never
// finishing.
std::atomic<unsigned long long> g_reads{0};

void count_read() {
    static const bool counting = [] {
        char const* opt = std::getenv("BG3LE_COUNT_READS");
        return opt != nullptr && opt[0] == '1';
    }();
    if (!counting) return;

    const unsigned long long n = g_reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 20000000ull != 0) return;

    static auto started = std::chrono::steady_clock::now();
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now()
                                      - started).count();
    logf("mem: %llu million fault-tolerant reads so far, %.0f per second",
         n / 1000000ull, seconds > 0 ? (double)n / seconds : 0.0);
}

bool read_raw(const void* addr, void* out, std::size_t n) {
    if (addr == nullptr || reinterpret_cast<std::uintptr_t>(addr) < 0x1000) return false;
    count_read();
    iovec local{out, n};
    iovec remote{const_cast<void*>(addr), n};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(n);
}
}  // namespace

bool safe_read(const void* addr, void* out, std::size_t n) {
    return read_raw(addr, out, n);
}

bool safe_write(void* addr, const void* in, std::size_t n) {
    iovec local{const_cast<void*>(in), n};
    iovec remote{addr, n};
    return ::process_vm_writev(::getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(n);
}

std::size_t safe_read_some(const void* addr, void* out, std::size_t n) {
    if (addr == nullptr || reinterpret_cast<std::uintptr_t>(addr) < 0x1000) {
        return 0;
    }
    iovec local{out, n};
    iovec remote{const_cast<void*>(addr), n};
    const ssize_t got =
        ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0);
    return got > 0 ? (std::size_t)got : 0;
}

bool safe_cstr(const void* addr, char* buf, std::size_t buf_size) {
    if (buf_size == 0) return false;
    buf[0] = '\0';

    // Read in page-friendly chunks so a string near a mapping edge still works.
    std::size_t got = 0;
    while (got < buf_size - 1) {
        std::size_t chunk = buf_size - 1 - got;
        if (chunk > 32) chunk = 32;
        if (!read_raw(static_cast<const char*>(addr) + got, buf + got, chunk)) break;
        if (std::memchr(buf + got, '\0', chunk) != nullptr) return true;
        got += chunk;
    }
    if (got == 0) return false;
    buf[got] = '\0';
    return true;
}

}  // namespace bg3le

void scan_yield() {
    // One millisecond every sixteen chunks -- sixteen megabytes. Over a
    // four gigabyte scan that is a quarter of a second added, and it is
    // the difference between the game ticking and not.
    static thread_local unsigned chunks = 0;
    if (++chunks % 16 != 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}

namespace {
thread_local bool g_scan_allowed = false;
}

void scan_enable_on_this_thread() { g_scan_allowed = true; }

bool scan_allowed() { return g_scan_allowed; }
