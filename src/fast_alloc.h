// Lock-free replacement for PhysX's TempAllocator.
//
// TempAllocator::allocate takes a single global MutexImpl, and during a level
// load seven worker threads push ~24M allocate/free pairs through it. Timing
// shows 99.6% of all conversion time is spent inside those two calls, so the
// lock -- not the conversion work -- is what makes loads take 65-100s.
//
// Blocks within a size class are interchangeable, so no ownership tracking is
// needed: a block freed on any thread joins that thread's free list and is
// reused there. The fast path touches only thread-local state.
#pragma once

#include <cstddef>

namespace bg3le {

// Replaces TempAllocator::allocate / ::deallocate. Opt in with
// BG3LE_FAST_ALLOC=1.
void fast_alloc_install();

// Blocks served, blocks recycled, and bytes of arena claimed.
void fast_alloc_report(const char* when);

}  // namespace bg3le
