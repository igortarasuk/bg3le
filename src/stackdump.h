// In-process stack sampling.
//
// The load stall parks every thread in futex_wait, but ptrace_scope=1 blocks
// gdb from attaching and eu-stack cannot unwind past libc. We are already
// inside the process, so signal each thread and let it record its own
// backtrace -- .eh_frame is intact, so the unwind reaches the game's frames.
#pragma once

namespace bg3le {

// Dumps every thread's backtrace to the log. Call from a normal thread
// context, not a signal handler. NOTE: skips the calling thread itself
// (it can't signal itself and wait synchronously) -- use dump_own_stack()
// for that.
void dump_all_thread_stacks(const char* reason);

// Dumps the calling thread's own backtrace directly (no signal dance
// needed -- we're already on the thread we want). This is what actually
// shows who called into a hook installed on the current call path, e.g.
// who called ISteamUserStats::SetAchievement.
void dump_own_stack(const char* reason);

// Arms a one-shot dump `delay_seconds` from now, on a detached thread.
void schedule_stack_dump(double delay_seconds, const char* reason);

}  // namespace bg3le
