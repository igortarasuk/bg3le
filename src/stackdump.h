// In-process stack sampling.
//
// The load stall parks every thread in futex_wait, but ptrace_scope=1 blocks
// gdb from attaching and eu-stack cannot unwind past libc. We are already
// inside the process, so signal each thread and let it record its own
// backtrace -- .eh_frame is intact, so the unwind reaches the game's frames.
#pragma once

namespace bg3le {

// Dumps every thread's backtrace to the log. Call from a normal thread
// context, not a signal handler.
void dump_all_thread_stacks(const char* reason);

// Arms a one-shot dump `delay_seconds` from now, on a detached thread.
void schedule_stack_dump(double delay_seconds, const char* reason);

}  // namespace bg3le
