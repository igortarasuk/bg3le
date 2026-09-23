// Recovers the engine's own static pointers, so a manager is read rather
// than searched for.
//
// Everything bg3le finds -- the stats manager, the module list, the
// template and prototype managers -- it finds by scanning the heap for
// something that looks right. That works, but only once the data exists,
// and it costs a scan. bg3se has neither problem: it pattern-matches the
// executable at startup to recover the addresses of the statics the engine
// keeps those pointers in, and from then on it just dereferences them.
//
// The Linux build gives no way to pattern-match for those statics -- of
// Larian's own code nothing carries a symbol, and its statics are
// anonymous -- but it does allow the inverse. Once a structure has been
// found by content, the static that points at it can be found by searching
// the executable's own writable data for its address. That static's offset
// within the image is a property of the build, not of the run, so it is
// recorded and reused: every later run reads the pointer directly, before
// anything has asked for it, with no scanning at all.
//
// The cache is keyed by the executable's build id, so a game patch
// invalidates it rather than silently resolving to the wrong offset.

#include <stdafx.h>

#include <cerrno>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "../log.h"
#include "../mem.h"

namespace bg3le {

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace {

std::string const& exe_path() {
    static std::string path;
    if (!path.empty()) return path;

    char buffer[4096];
    const ssize_t n = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (n > 0) {
        buffer[n] = '\0';
        path = buffer;
    }
    return path;
}

// The executable's own writable mappings -- .data and .bss -- which is
// where a static pointer lives. Nothing else is searched: a manager
// pointer is not in the heap, and searching everything would defeat the
// point of doing this at all.
std::vector<std::pair<unsigned long long, unsigned long long>>
writable_image_regions() {
    std::vector<std::pair<unsigned long long, unsigned long long>> out;
    if (exe_path().empty()) return out;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return out;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe_path().c_str()) == nullptr) continue;

        unsigned long long from = 0;
        unsigned long long to = 0;
        char perms[8] = {};
        if (std::sscanf(line, "%llx-%llx %7s", &from, &to, perms) != 3) {
            continue;
        }
        if (std::strchr(perms, 'w') == nullptr) continue;
        out.emplace_back(from, to);
    }
    std::fclose(maps);
    return out;
}

// The lowest address the executable is mapped at, so an offset recorded in
// one run means the same thing in the next.
std::uintptr_t image_base() {
    static std::uintptr_t base = 0;
    if (base != 0) return base;
    if (exe_path().empty()) return 0;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return 0;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe_path().c_str()) == nullptr) continue;
        unsigned long long from = 0;
        if (std::sscanf(line, "%llx", &from) != 1) continue;
        if (base == 0 || from < base) base = (std::uintptr_t)from;
    }
    std::fclose(maps);
    return base;
}

// Identifies the build, so a game patch invalidates the cache instead of
// resolving to an offset that has moved.
std::string build_key() {
    if (exe_path().empty()) return {};
    struct stat st{};
    if (stat(exe_path().c_str(), &st) != 0) return {};

    char key[64];
    std::snprintf(key, sizeof(key), "%llx-%llx",
                  (unsigned long long)st.st_size,
                  (unsigned long long)st.st_mtime);
    return key;
}

// What a search found, expressed so it can be recovered next run: the
// static's offset within the image, and how far past that pointer the
// thing we actually wanted sits.
//
// The delta matters because the searches land inside a manager, not at its
// base -- the stats search anchors on a run of treasure rarities partway
// through RPGStats -- while the static holds the base. Recording the gap
// means neither the struct's layout nor the manager's size has to be
// known.
struct Entry {
    std::uint64_t Offset{0};   // of the static, within the image
    std::uint64_t Delta{0};    // from the pointer to what was wanted
};

struct Cache {
    bool Loaded{false};
    bool Dirty{false};
    std::string Path;
    // Several candidates per key, nearest first. Which static the engine
    // actually keeps the manager in is not decidable from one run -- any
    // static pointing into the same arena looks the same -- so all of them
    // are kept and the caller validates until one holds.
    std::map<std::string, std::vector<Entry>> Offsets;
};

Cache& cache() {
    static Cache c;
    return c;
}

std::string cache_path() {
    char const* home = std::getenv("HOME");
    const std::string build = build_key();
    if (home == nullptr || build.empty()) return {};

    const std::string dir = std::string(home) + "/.local/share/bg3le";
    mkdir(dir.c_str(), 0755);
    return dir + "/statics-" + build + ".txt";
}

void load_cache() {
    Cache& c = cache();
    if (c.Loaded) return;
    c.Loaded = true;
    c.Path = cache_path();
    if (c.Path.empty()) return;

    std::FILE* f = std::fopen(c.Path.c_str(), "r");
    if (f == nullptr) return;

    char line[4096];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char key[128];
        int consumed = 0;
        if (std::sscanf(line, "%127s%n", key, &consumed) != 1) continue;

        std::vector<Entry> entries;
        char const* at = line + consumed;
        for (;;) {
            unsigned long long offset = 0;
            unsigned long long delta = 0;
            int used = 0;
            if (std::sscanf(at, " %llx:%llx%n", &offset, &delta, &used)
                != 2) {
                break;
            }
            entries.push_back(Entry{offset, delta});
            at += used;
        }
        if (!entries.empty()) c.Offsets[key] = std::move(entries);
    }
    std::fclose(f);
    logf("statics: %zu cached offsets from %s", c.Offsets.size(),
         c.Path.c_str());
}

void save_cache() {
    Cache& c = cache();
    if (!c.Dirty || c.Path.empty()) return;

    std::FILE* f = std::fopen(c.Path.c_str(), "w");
    if (f == nullptr) return;
    for (auto const& entry : c.Offsets) {
        std::fprintf(f, "%s", entry.first.c_str());
        for (Entry const& candidate : entry.second) {
            std::fprintf(f, " %llx:%llx",
                         (unsigned long long)candidate.Offset,
                         (unsigned long long)candidate.Delta);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
    c.Dirty = false;
}

}  // namespace

// Reads a pointer whose static bg3le has already recorded, or null.
//
// This is the fast path, and the whole point: no scan, no waiting, usable
// the moment the engine has filled the static in.
extern "C" void* bg3le_static_get(char const* key, std::size_t which) {
    load_cache();
    if (key == nullptr) return nullptr;

    auto it = cache().Offsets.find(key);
    if (it == cache().Offsets.end() || which >= it->second.size()) {
        return nullptr;
    }

    const std::uintptr_t base = image_base();
    if (base == 0) return nullptr;

    Entry const& entry = it->second[which];
    void* value = nullptr;
    if (!safe_read((void const*)(base + entry.Offset), &value,
                   sizeof(value))) {
        return nullptr;
    }
    if (value == nullptr) return nullptr;   // the engine has not filled it in
    return (void*)((char*)value + entry.Delta);
}

// How many candidates were recorded for a key.
extern "C" std::size_t bg3le_static_count(char const* key) {
    load_cache();
    if (key == nullptr) return 0;
    auto it = cache().Offsets.find(key);
    return it == cache().Offsets.end() ? 0 : it->second.size();
}

// Narrows a key to the candidate that worked, so later runs try it first
// and the rest are forgotten.
extern "C" void bg3le_static_confirm(char const* key, std::size_t which) {
    load_cache();
    if (key == nullptr) return;

    auto it = cache().Offsets.find(key);
    if (it == cache().Offsets.end() || which >= it->second.size()) return;
    if (it->second.size() == 1) return;

    Entry const kept = it->second[which];
    it->second.assign(1, kept);
    cache().Dirty = true;
    save_cache();
    logf("statics: %s confirmed at image+%#llx plus %#llx", key,
         (unsigned long long)kept.Offset, (unsigned long long)kept.Delta);
}

// Records where the engine keeps a pointer to an object bg3le has already
// found, so later runs can skip the search.
//
// Every candidate is checked by reading it back: the offset only counts if
// the word at it really holds this address. Several statics can hold the
// same pointer; the first is taken, and being wrong about which is
// harmless since they are interchangeable by definition.
extern "C" bool bg3le_static_record(char const* key, void const* object) {
    load_cache();
    if (key == nullptr || object == nullptr) return false;
    if (cache().Offsets.count(key) != 0) return true;

    const std::uintptr_t base = image_base();
    if (base == 0) return false;

    // The static holds a pointer at or before what was found. A manager
    // is not megabytes wide, so a window keeps an unrelated pointer that
    // happens to sit below it from being mistaken for one -- and the
    // closest candidate is taken, which is the innermost enclosing object.
    // Wide enough for a manager the size of RPGStats, whose rarity run
    // sits 55KB into it.
    constexpr std::uint64_t kWindow = 1u << 20;
    constexpr std::size_t kMaxCandidates = 24;

    const auto wanted = (std::uint64_t)(std::uintptr_t)object;
    std::vector<Entry> candidates;

    constexpr std::size_t kChunk = 1u << 20;
    std::vector<unsigned char> block(kChunk + 8);

    for (auto const& region : writable_image_regions()) {
        for (unsigned long long at = region.first; at < region.second;
             at += kChunk) {
            std::size_t want = (std::size_t)(region.second - at);
            if (want > kChunk + 8) want = kChunk + 8;
            const std::size_t got =
                safe_read_some((void const*)at, block.data(), want);
            if (got < 8) continue;

            for (std::size_t off = 0; off + 8 <= got; off += 8) {
                std::uint64_t word = 0;
                std::memcpy(&word, block.data() + off, sizeof(word));
                if (word == 0 || word > wanted) continue;
                if (wanted - word >= kWindow) continue;

                candidates.push_back(
                    Entry{(at + off) - base, wanted - word});
            }
        }
    }

    if (!candidates.empty()) {
        // Nearest first: the innermost enclosing object is the likeliest,
        // and the caller stops at the first that validates.
        std::sort(candidates.begin(), candidates.end(),
                  [](Entry const& a, Entry const& b) {
                      return a.Delta < b.Delta;
                  });
        if (candidates.size() > kMaxCandidates) {
            candidates.resize(kMaxCandidates);
        }

        cache().Offsets[key] = candidates;
        cache().Dirty = true;
        save_cache();
        logf("statics: %s has %zu candidate statics, nearest at image+%#llx "
             "plus %#llx", key, candidates.size(),
             (unsigned long long)candidates[0].Offset,
             (unsigned long long)candidates[0].Delta);
        return true;
    }

    logf("statics: nothing in the image points at %s; it will keep being "
         "searched for", key);
    return false;
}

}  // namespace bg3le
