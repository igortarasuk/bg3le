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
// Writes into our own memory without risking a fault on a bad pointer,
// the counterpart to safe_read. Used only where the target has already
// been identified exactly -- see src/vendor/version_text.cpp.
bool safe_write(void* addr, const void* in, std::size_t n);

// For scanning rather than probing. safe_read costs one process_vm_readv
// syscall per call, so checking a candidate at every offset of a writable
// region means hundreds of millions of syscalls and never finishes; reading a
// region in large blocks and searching the copy costs one syscall per block.
// A short read is normal here, since a region can have unmapped pages inside
// it, so the count is returned rather than being treated as failure.
std::size_t safe_read_some(const void* addr, void* out, std::size_t n);

}  // namespace bg3le

// Called once per chunk by anything scanning the whole address space.
//
// These scans run on a background thread but they are not free to the rest
// of the process: every read is a process_vm_readv, which takes the mmap
// lock, and millions of them back to back leave the game blocked on its
// own allocations -- its threads sitting idle rather than busy, unable to
// tick at all. Pausing briefly every so often lets that drain.
void scan_yield();

// Whole-address-space scans are allowed on the warming thread and nowhere
// else.
//
// Without this a search that has not finished yet gets run by whichever
// thread asks first -- and that is the story thread, which then spends a
// minute in process_vm_readv while the game stops ticking. A search that
// is not ready reports itself unavailable instead, and the warming thread
// fills it in.
void scan_enable_on_this_thread();
bool scan_allowed();
