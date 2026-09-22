// Stops the engine from pinning its threads onto shared physical cores.
//
// The engine sets thread affinity itself, one logical CPU per thread: on a
// 7840U it reserves cpu 0/2/4/6 for the main thread, GameThread, ServerWorker
// and WorkerThreadSch, and puts its worker pool on 1,3,5,7-15. The siblings
// here are (0,1) (2,3) (4,5) (6,7), so those workers land on the very physical
// cores the reservation was meant to protect. Measured: cpu0 at 80.6% with its
// sibling at 63.2%, the main thread waiting 3.07ms in a median 50ms window and
// up to 33ms in the worst, and 48.6% of windows showing some thread ready but
// with no CPU to run on -- while cpu 11/13/15 had nothing pinned to them at
// all.
//
// Doing this here rather than in the launch script is not a preference. The
// engine pins each thread as it creates it, including threads it creates
// during play, so a taskset at launch is overwritten moments later. We are
// already in the process, so we can answer the call instead.
//
// The policy takes the engine's intent at face value and only fixes the
// collision. A single-CPU pin claims that CPU's whole physical core for the
// caller; a later request that would land on an already-claimed core is sent
// to the cores nobody has claimed. Nothing is hardcoded about which CPUs those
// are, so a different topology rearranges itself rather than inheriting this
// machine's numbers.
//
// After both rewrites, applied by hand to a running game: cpu1 8.9%, cpu3
// 4.4%, cpu5 4.2%, cpu7 8.9%, the main thread's median wait 0.01ms and worst
// 2.16ms, and no window in 395 with a thread waiting on a runqueue. Total
// system CPU fell 47.4% -> 40.3%, the work that had been going into
// contention. Frametimes improved slightly.
//
// What this does not fix: the cutscene stutter, which happens in other games
// too and is a separate problem. It also cannot lift the ceiling that remains
// once the contention is gone -- a pinned thread still gets one core, and the
// main thread was measured at 87% of one, so it is near that limit either way.
//
// BG3LE_AFFINITY=off disables it; =free ignores the engine's masks entirely
// and lets the kernel place everything.

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace {

using bg3le::logf;

enum class Mode { Spread, Free, Off };

Mode mode() {
    static const Mode m = [] {
        const char* opt = std::getenv("BG3LE_AFFINITY");
        if (opt == nullptr) return Mode::Spread;
        if (std::strcmp(opt, "off") == 0) return Mode::Off;
        if (std::strcmp(opt, "free") == 0) return Mode::Free;
        return Mode::Spread;
    }();
    return m;
}

// ---- topology ----

struct Topology {
    int CpuCount{0};
    // Which physical core each logical cpu belongs to, and the cpus of each.
    std::vector<int> CoreOfCpu;
    std::vector<std::vector<int>> CpusOfCore;
};

// Parses "0-1" or "0,8" as /sys writes a sibling list.
void parse_list(const char* text, std::vector<int>* out) {
    const char* p = text;
    while (*p != '\0') {
        char* end = nullptr;
        const long first = std::strtol(p, &end, 10);
        if (end == p) break;
        long last = first;
        if (*end == '-') {
            p = end + 1;
            last = std::strtol(p, &end, 10);
        }
        for (long c = first; c <= last; ++c) out->push_back((int)c);
        if (*end == ',') {
            p = end + 1;
        } else {
            break;
        }
    }
}

Topology const& topology() {
    static const Topology topo = [] {
        Topology t;
        t.CpuCount = (int)::sysconf(_SC_NPROCESSORS_CONF);
        if (t.CpuCount <= 0) t.CpuCount = CPU_SETSIZE;
        t.CoreOfCpu.assign((std::size_t)t.CpuCount, -1);

        for (int cpu = 0; cpu < t.CpuCount; ++cpu) {
            if (t.CoreOfCpu[(std::size_t)cpu] != -1) continue;

            char path[128];
            std::snprintf(path, sizeof(path),
                          "/sys/devices/system/cpu/cpu%d/topology/"
                          "thread_siblings_list",
                          cpu);
            std::vector<int> siblings;
            if (std::FILE* f = std::fopen(path, "r")) {
                char buf[256] = {0};
                if (std::fgets(buf, sizeof(buf), f) != nullptr) {
                    parse_list(buf, &siblings);
                }
                std::fclose(f);
            }
            // A kernel that will not tell us treats the cpu as its own core,
            // which costs the SMT pairing and nothing else.
            if (siblings.empty()) siblings.push_back(cpu);

            const int core = (int)t.CpusOfCore.size();
            std::vector<int> members;
            for (int s : siblings) {
                if (s < 0 || s >= t.CpuCount) continue;
                t.CoreOfCpu[(std::size_t)s] = core;
                members.push_back(s);
            }
            t.CpusOfCore.push_back(members);
        }
        return t;
    }();
    return topo;
}

// ---- claims ----

std::mutex& lock() {
    static std::mutex m;
    return m;
}

// core -> the token that claimed it. One claim per core; the first single-cpu
// pin wins and later ones are moved aside.
std::unordered_map<int, unsigned long long>& claims() {
    static std::unordered_map<int, unsigned long long> m;
    return m;
}

std::vector<int> cpus_of(const cpu_set_t* set, std::size_t size) {
    std::vector<int> out;
    const int count = topology().CpuCount;
    for (int c = 0; c < count; ++c) {
        if (CPU_ISSET_S((std::size_t)c, size, set)) out.push_back(c);
    }
    return out;
}

std::string render(std::vector<int> const& cpus) {
    std::string s;
    for (std::size_t i = 0; i < cpus.size(); ++i) {
        if (i != 0) s += ",";
        s += std::to_string(cpus[i]);
    }
    return s.empty() ? std::string("(empty)") : s;
}

// Every cpu on a core nobody has claimed.
std::vector<int> unclaimed_cpus() {
    std::vector<int> out;
    auto const& topo = topology();
    for (std::size_t core = 0; core < topo.CpusOfCore.size(); ++core) {
        if (claims().count((int)core) != 0) continue;
        for (int c : topo.CpusOfCore[core]) out.push_back(c);
    }
    return out;
}

// The rewritten cpu list, or empty to pass the request through untouched.
// Caller holds the lock.
std::vector<int> decide(std::vector<int> const& want,
                        unsigned long long token) {
    auto const& topo = topology();
    if (want.empty()) return {};

    if (want.size() == 1) {
        const int cpu = want[0];
        if (cpu < 0 || cpu >= topo.CpuCount) return {};
        const int core = topo.CoreOfCpu[(std::size_t)cpu];
        if (core < 0) return {};

        auto it = claims().find(core);
        if (it == claims().end()) {
            // Free core: hand over the whole thing, siblings included, so the
            // thread is not boxed onto one hardware thread of it.
            claims()[core] = token;
            return topo.CpusOfCore[(std::size_t)core];
        }
        if (it->second == token) {
            return topo.CpusOfCore[(std::size_t)core];
        }
        // Someone else's core. Anywhere unclaimed beats sharing it.
        return unclaimed_cpus();
    }

    // A wide mask: drop the cpus of cores that are already claimed, as long as
    // that leaves the thread somewhere to run.
    std::vector<int> kept;
    for (int cpu : want) {
        if (cpu < 0 || cpu >= topo.CpuCount) continue;
        const int core = topo.CoreOfCpu[(std::size_t)cpu];
        auto it = claims().find(core);
        if (it != claims().end() && it->second != token) continue;
        kept.push_back(cpu);
    }
    if (kept.empty() || kept.size() == want.size()) return {};
    return kept;
}

// The name of the thread being repinned, which is not always the caller: a
// pool usually pins its workers from whichever thread created them. Logging
// the caller's name instead would attribute every move to one thread.
char const* thread_name(char* buf, std::size_t capacity, const pthread_t* via,
                        pid_t tid) {
    buf[0] = '\0';
    if (via != nullptr) {
        if (::pthread_getname_np(*via, buf, capacity) == 0) return buf;
        buf[0] = '\0';
        return buf;
    }

    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", (int)tid);
    if (std::FILE* f = std::fopen(path, "r")) {
        if (std::fgets(buf, (int)capacity, f) != nullptr) {
            const std::size_t n = std::strlen(buf);
            if (n != 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
        }
        std::fclose(f);
    }
    return buf;
}

using SchedFn = int (*)(pid_t, size_t, const cpu_set_t*);
using PthreadFn = int (*)(pthread_t, size_t, const cpu_set_t*);

// Applies the policy and returns the set to install, or null to pass through.
// Writes into scratch, which the caller keeps alive across the real call.
const cpu_set_t* rewrite(const cpu_set_t* mask, std::size_t size,
                         unsigned long long token, const char* api,
                         const pthread_t* via, pid_t tid,
                         cpu_set_t* scratch) {
    if (mode() == Mode::Off || mask == nullptr) return nullptr;

    const std::vector<int> want = cpus_of(mask, size);

    std::vector<int> give;
    if (mode() == Mode::Free) {
        // Only worth saying anything if the engine was actually narrowing.
        if ((int)want.size() >= topology().CpuCount) return nullptr;
        for (int c = 0; c < topology().CpuCount; ++c) give.push_back(c);
    } else {
        std::lock_guard<std::mutex> guard(lock());
        give = decide(want, token);
    }
    if (give.empty()) return nullptr;

    CPU_ZERO_S(size, scratch);
    for (int c : give) CPU_SET_S((std::size_t)c, size, scratch);

    char name[32];
    logf("affinity: %s wanted cpu %s for \"%s\", gave it %s", api,
         render(want).c_str(), thread_name(name, sizeof(name), via, tid),
         render(give).c_str());
    return scratch;
}

}  // namespace

// The engine calls these directly; we are earlier in the search order, so
// defining them here interposes and dlsym(RTLD_NEXT) reaches the real ones.

extern "C" int sched_setaffinity(pid_t pid, size_t cpusetsize,
                                 const cpu_set_t* mask) {
    static const auto real =
        reinterpret_cast<SchedFn>(::dlsym(RTLD_NEXT, "sched_setaffinity"));
    if (real == nullptr) return -1;

    const pid_t tid = pid == 0 ? ::gettid() : pid;

    cpu_set_t scratch;
    if (const cpu_set_t* use =
            rewrite(mask, cpusetsize, (unsigned long long)tid,
                    "sched_setaffinity", nullptr, tid, &scratch)) {
        return real(pid, cpusetsize, use);
    }
    return real(pid, cpusetsize, mask);
}

extern "C" int pthread_setaffinity_np(pthread_t thread, size_t cpusetsize,
                                      const cpu_set_t* mask) {
    static const auto real = reinterpret_cast<PthreadFn>(
        ::dlsym(RTLD_NEXT, "pthread_setaffinity_np"));
    if (real == nullptr) return -1;

    // A thread setting its own affinity is the common case, and using the tid
    // for it keeps one thread to one token even if it also calls the sched
    // interface.
    const bool isSelf = ::pthread_equal(thread, ::pthread_self()) != 0;
    const unsigned long long token = isSelf
                                         ? (unsigned long long)::gettid()
                                         : (unsigned long long)thread;

    cpu_set_t scratch;
    if (const cpu_set_t* use =
            rewrite(mask, cpusetsize, token, "pthread_setaffinity_np", &thread,
                    isSelf ? ::gettid() : 0, &scratch)) {
        return real(thread, cpusetsize, use);
    }
    return real(thread, cpusetsize, mask);
}
