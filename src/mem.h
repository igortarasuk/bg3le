// Fault-tolerant reads for probing unknown engine memory.
#pragma once

#include <cstddef>

namespace bg3le {

// Reads n bytes from addr, returning false instead of faulting.
bool safe_read(const void* addr, void* out, std::size_t n);

// Copies a NUL-terminated string from addr into buf; returns false if addr
// is unreadable or holds no terminator within the buffer.
bool safe_cstr(const void* addr, char* buf, std::size_t buf_size);

}  // namespace bg3le
