#include "mem.h"

#include <sys/uio.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>

namespace bg3le {
namespace {
// Reading our own memory through process_vm_readv turns a bad pointer into
// EFAULT rather than SIGSEGV, which matters when probing engine structs.
bool read_raw(const void* addr, void* out, std::size_t n) {
    if (addr == nullptr || reinterpret_cast<std::uintptr_t>(addr) < 0x1000) return false;
    iovec local{out, n};
    iovec remote{const_cast<void*>(addr), n};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) ==
           static_cast<ssize_t>(n);
}
}  // namespace

bool safe_read(const void* addr, void* out, std::size_t n) {
    return read_raw(addr, out, n);
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
