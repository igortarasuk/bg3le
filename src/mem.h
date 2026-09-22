// Fault-tolerant reads for probing unknown engine memory.
#pragma once

#include <cstddef>

namespace bg3le {

// Reads n bytes from addr, returning false instead of faulting.
bool safe_read(const void* addr, void* out, std::size_t n);

// Copies a NUL-terminated string from addr into buf; returns false if addr
// is unreadable or holds no terminator within the buffer.
bool safe_cstr(const void* addr, char* buf, std::size_t buf_size);

// Reads up to n bytes, returning how many were actually read.
//
// For scanning rather than probing. safe_read costs one process_vm_readv
// syscall per call, so checking a candidate at every offset of a writable
// region means hundreds of millions of syscalls and never finishes; reading a
// region in large blocks and searching the copy costs one syscall per block.
// A short read is normal here, since a region can have unmapped pages inside
// it, so the count is returned rather than being treated as failure.
std::size_t safe_read_some(const void* addr, void* out, std::size_t n);

}  // namespace bg3le
