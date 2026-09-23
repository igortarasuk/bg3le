#include "stackdump.h"

#include <dirent.h>
#include <execinfo.h>
#include <link.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "log.h"

namespace bg3le {
namespace {

constexpr int kMaxFrames = 24;

// SIGRTMIN is a function call in glibc, so this cannot be constexpr.
int dump_signal() { return SIGRTMIN + 3; }

struct Slot {
    void* frames[kMaxFrames];
    int count;
};

Slot g_slot;
sem_t g_done;
std::atomic<bool> g_installed{false};

void handler(int) {
    // backtrace() is not formally async-signal-safe, but it is the standard
    // way to do this and the alternative is no data at all.
    g_slot.count = ::backtrace(g_slot.frames, kMaxFrames);
    ::sem_post(&g_done);
}

std::vector<long> thread_ids() {
    std::vector<long> out;
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return out;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        out.push_back(std::strtol(e->d_name, nullptr, 10));
    }
    ::closedir(d);
    return out;
}

std::string thread_name(long tid) {
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/comm", tid);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return "?";
    char buf[64] = {0};
    if (std::fgets(buf, sizeof(buf), f) == nullptr) buf[0] = '\0';
    std::fclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

std::string wchan(long tid) {
    char path[128];
    std::snprintf(path, sizeof(path), "/proc/self/task/%ld/wchan", tid);
    std::FILE* f = std::fopen(path, "r");
    if (f == nullptr) return "?";
    char buf[64] = {0};
    if (std::fread(buf, 1, sizeof(buf) - 1, f) == 0) buf[0] = '\0';
    std::fclose(f);
    return buf;
}

std::uintptr_t main_bias() {
    std::uintptr_t bias = 0;
    ::dl_iterate_phdr(
        [](struct dl_phdr_info* info, std::size_t, void* data) {
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
            *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
            return 1;
        },
        &bias);
    return bias;
}

}  // namespace

void dump_all_thread_stacks(const char* reason) {
    bool expected = false;
    if (g_installed.compare_exchange_strong(expected, true)) {
        ::sem_init(&g_done, 0, 0);
        struct sigaction sa {};
        sa.sa_handler = handler;
        sa.sa_flags = SA_RESTART;
        ::sigemptyset(&sa.sa_mask);
        ::sigaction(dump_signal(), &sa, nullptr);
    }

    const std::uintptr_t bias = main_bias();
    const long self = ::syscall(SYS_gettid);
    logf("stackdump (%s): bias 0x%lx -- offsets below are link-time for bg3",
         reason, (unsigned long)bias);

    for (long tid : thread_ids()) {
        const std::string name = thread_name(tid);
        const std::string where = wchan(tid);
        if (tid == self) continue;

        g_slot.count = 0;
        if (::syscall(SYS_tgkill, ::getpid(), tid, dump_signal()) != 0) continue;

        timespec deadline {};
        ::clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += 2;
        if (::sem_timedwait(&g_done, &deadline) != 0) {
            logf("  tid %ld %-18s wchan=%s (no response)", tid, name.c_str(),
                 where.c_str());
            continue;
        }

        std::string line;
        char buf[32];
        for (int i = 0; i < g_slot.count; ++i) {
            const auto addr = reinterpret_cast<std::uintptr_t>(g_slot.frames[i]);
            // Report bg3-relative offsets so they resolve against the binary.
            std::snprintf(buf, sizeof(buf), " %#lx",
                          (unsigned long)(addr >= bias ? addr - bias : addr));
            line += buf;
        }
        logf("  tid %ld %-18s wchan=%-14s%s", tid, name.c_str(), where.c_str(),
             line.c_str());
    }
    logf("stackdump (%s): end", reason);
}

void dump_own_stack(const char* reason) {
    const std::uintptr_t bias = main_bias();
    void* frames[kMaxFrames];
    int count = ::backtrace(frames, kMaxFrames);
    std::string line;
    char buf[32];
    for (int i = 0; i < count; ++i) {
        const auto addr = reinterpret_cast<std::uintptr_t>(frames[i]);
        std::snprintf(buf, sizeof(buf), " %#lx",
                      (unsigned long)(addr >= bias ? addr - bias : addr));
        line += buf;
    }
    logf("ownstack (%s): bias 0x%lx%s", reason, (unsigned long)bias, line.c_str());
}

void schedule_stack_dump(double delay_seconds, const char* reason) {
    const std::string why(reason);
    std::thread([delay_seconds, why] {
        timespec ts {};
        ts.tv_sec = (time_t)delay_seconds;
        ts.tv_nsec = (long)((delay_seconds - (double)ts.tv_sec) * 1e9);
        ::nanosleep(&ts, nullptr);
        dump_all_thread_stacks(why.c_str());
    }).detach();
}

}  // namespace bg3le
