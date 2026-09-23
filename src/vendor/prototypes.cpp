// The spell and status prototype managers, which is what
// Ext.Stats.GetCachedSpell and GetCachedStatus read.
//
// A prototype is the engine's parsed form of a stat: the stats object says
// what the .txt file said, the prototype is what the engine actually runs.
// Neither manager has a symbol.
//
// Both are found the way the templates were -- by validating a candidate
// against its own contents rather than by recognising a container's shape.
// Each manager holds HashMap<FixedString, Prototype*>, and a prototype
// carries the same name the map is keyed by, so for a genuine map every
// entry satisfies
//
//     *(FixedString*)(Values[i] + nameOffset) == Keys[i]
//
// Eight consecutive entries agreeing on that is not something another map
// does by accident, and it is a statement about the data rather than about
// the layout.

#include <stdafx.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" void* bg3le_stats_find(char const* name);
extern "C" char const* bg3le_stats_type(void const* object);
extern "C" void* bg3le_static_get(char const* key, std::size_t which);
extern "C" std::size_t bg3le_static_count(char const* key);
extern "C" void bg3le_static_confirm(char const* key, std::size_t which);
extern "C" bool bg3le_static_record_path(char const* key, void const* target,
                                         std::uint64_t first_window);

namespace {

// A bg3se HashMap: HashKeys, NextIds, Keys, then Values.
constexpr std::size_t kKeysBuffer = 32;
constexpr std::size_t kKeysCapacity = 40;
constexpr std::size_t kKeysSize = 44;
constexpr std::size_t kValuesBuffer = 48;
constexpr std::size_t kMapSize = 64;

// Array<FixedString>, so four bytes per key -- not the eight a
// RuntimeStringHandle takes.
constexpr std::size_t kKeyStride = 4;
constexpr std::size_t kValueStride = 8;   // Prototype*

constexpr std::uint32_t kNullFixedString = 0xffffffffu;

// How many entries have to agree before a candidate is believed, and how
// many a real manager holds at the least.
constexpr std::uint32_t kAgreeing = 8;
// Low enough for the smaller managers: the interrupt map holds hundreds
// where the spell map holds thousands.
constexpr std::uint32_t kMinEntries = 50;
constexpr std::uint32_t kMaxEntries = 1u << 20;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

struct Kind {
    char const* Name;        // for the log
    std::size_t NameOffset;  // where the prototype repeats its own name
};

// SpellPrototype opens with StatsObjectIndex and SpellTypeId, then SpellId;
// StatusPrototype with StatsObjectIndex and StatusId, then StatusName. Both
// repeat their name at the same offset, which is why the self-check alone
// cannot tell the two managers apart -- see kind_of below.
constexpr Kind kKinds[] = {
    {"spell", 8},
    {"status", 8},
    // Interrupts and passives are stored in their map rather than behind a
    // pointer, so what varies is the stride between them; the name still
    // has to match the key, which is what derives it.
    {"interrupt", 0},
    // Passives are a LegacyRefMap -- chained nodes rather than parallel
    // arrays -- so this scan cannot see them; the entry is kept so the
    // table indices stay stable.
    {"passive", 4},
};
constexpr std::size_t kSpell = 0;
constexpr std::size_t kStatus = 1;
constexpr std::size_t kInterrupt = 2;
constexpr std::size_t kPassive = 3;

// Which kind a validated map holds, from the stats its names belong to: a
// spell prototype is named after a SpellData stat and a status after a
// StatusData one. Without this the two maps were labelled in the order
// they happened to be found, and they came out swapped -- GetCachedSpell
// was looking spells up in the status map.
//
// Unanimity is required. A split vote means the map is neither, and
// guessing from a majority is how you end up with one wrong entry that
// nobody notices.
int kind_of(void const* keys, std::uint32_t count) {
    std::size_t spells = 0;
    std::size_t statuses = 0;
    std::size_t asked = 0;

    constexpr std::size_t kVotes = 3;
    for (std::uint32_t i = 0; i < count && asked < kVotes; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) break;
        if (key == 0 || key == kNullFixedString) continue;

        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr) continue;

        void* object = bg3le_stats_find(name);
        if (object == nullptr) continue;
        char const* type = bg3le_stats_type(object);
        if (type == nullptr) continue;

        ++asked;
        if (std::strcmp(type, "SpellData") == 0) ++spells;
        else if (std::strcmp(type, "StatusData") == 0) ++statuses;
    }

    if (asked < kVotes) return -1;
    if (spells == asked) return (int)kSpell;
    if (statuses == asked) return (int)kStatus;
    return -1;
}

struct Table {
    std::unordered_map<std::string, std::uint64_t> ByName;
    std::vector<std::string> Order;
};

struct Prototypes {
    // Written by the warming thread and read by the story thread. The
    // flag is released after the tables are filled and acquired before
    // they are read, so a reader that sees it set sees the contents too --
    // without that the handoff is a data race and the reader can see an
    // empty map.
    std::atomic<bool> Built{false};
    Table Kinds[std::size(kKinds)];
};

Prototypes& state() {
    static Prototypes p;
    return p;
}

// Whether every one of the first few entries is a prototype that names
// itself the way the map is keyed.
bool self_consistent(void const* keys, void const* values,
                     std::uint32_t count, std::size_t nameOffset) {
    const std::uint32_t probe = count < kAgreeing ? count : kAgreeing;
    for (std::uint32_t i = 0; i < probe; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) return false;
        if (key == 0 || key == kNullFixedString) return false;

        void const* prototype = nullptr;
        if (!read_as((char const*)values + i * kValueStride, &prototype)
            || prototype == nullptr) {
            return false;
        }

        std::uint32_t own = 0;
        if (!read_as((char const*)prototype + nameOffset, &own)) return false;
        if (own != key) return false;
    }
    return probe == kAgreeing;
}

// The stride between inline prototypes, derived rather than assumed:
// sizeof(InterruptPrototype) as this build computes it is not the
// engine's, as every other struct here has shown. The right stride is the
// one where every sampled entry names itself the way its key does; a
// wrong one disagrees on the first.
std::size_t inline_stride(void const* keys, void const* values,
                          std::uint32_t count, std::size_t nameOffset) {
    const std::uint32_t probe = count < kAgreeing ? count : kAgreeing;
    if (probe < kAgreeing) return 0;

    for (std::size_t stride = 8; stride <= 512; stride += 8) {
        bool ok = true;
        for (std::uint32_t i = 0; i < probe && ok; ++i) {
            std::uint32_t key = 0;
            if (!read_as((char const*)keys + i * kKeyStride, &key)
                || key == 0 || key == kNullFixedString) {
                ok = false;
                break;
            }
            std::uint32_t own = 0;
            if (!read_as((char const*)values + i * stride + nameOffset,
                         &own)) {
                ok = false;
                break;
            }
            ok = own == key;
        }
        if (ok) return stride;
    }
    return 0;
}

std::size_t harvest_inline(void const* keys, void const* values,
                           std::uint32_t count, std::size_t nameOffset,
                           std::size_t stride, Table* into) {
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) break;
        if (key == 0 || key == kNullFixedString) continue;

        auto const* at = (char const*)values + i * stride;
        std::uint32_t own = 0;
        if (!read_as(at + nameOffset, &own) || own != key) continue;

        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr || name[0] == '\0') continue;

        if (into->ByName.emplace(name, (std::uint64_t)(std::uintptr_t)at)
                .second) {
            ++added;
        }
    }
    return added;
}

std::size_t harvest(void const* keys, void const* values,
                    std::uint32_t count, std::size_t nameOffset,
                    Table* into) {
    std::size_t added = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t key = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &key)) break;
        if (key == 0 || key == kNullFixedString) continue;

        void const* prototype = nullptr;
        if (!read_as((char const*)values + i * kValueStride, &prototype)
            || prototype == nullptr) {
            continue;
        }

        // Checked per entry, not just on the sample: a map that passed the
        // probe still must not contribute an entry that disagrees.
        std::uint32_t own = 0;
        if (!read_as((char const*)prototype + nameOffset, &own)) continue;
        if (own != key) continue;

        char const* name = bg3le_fixed_string(key, nullptr);
        if (name == nullptr || name[0] == '\0') continue;

        if (into->ByName.emplace(name, (std::uint64_t)(std::uintptr_t)
                                           prototype).second) {
            ++added;
        }
    }
    return added;
}

struct Candidate {
    void const* Keys{nullptr};
    void const* Values{nullptr};
    std::uint32_t Count{0};
    unsigned long long At{0};
};

// A validated prototype map is rare; this only exists so a pathological
// process cannot make the classification pass unbounded either.
constexpr std::size_t kMaxCandidates = 64;

// How many of them are actually classified, largest first.
constexpr std::size_t kClassify = 6;

// The key a path from a static to one kind's manager is recorded under.
std::string static_key(std::size_t k) {
    return std::string("prototypes.") + kKinds[k].Name;
}

// The map at `at`, validated as kind `k`'s manager and harvested. Shared
// between the scan and a recorded static so neither adopts what the other
// would reject.
bool harvest_map(unsigned long long at, std::size_t k, Table* into) {
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    std::uint64_t keysWord = 0;
    std::uint64_t valuesWord = 0;
    auto const* head = (char const*)(std::uintptr_t)at;
    if (!read_as(head + kKeysCapacity, &capacity)
        || !read_as(head + kKeysSize, &count)
        || !read_as(head + kKeysBuffer, &keysWord)
        || !read_as(head + kValuesBuffer, &valuesWord)) {
        return false;
    }
    if (count < kMinEntries || count > kMaxEntries || count > capacity) {
        return false;
    }

    auto const* keys = (void const*)(std::uintptr_t)keysWord;
    auto const* values = (void const*)(std::uintptr_t)valuesWord;

    if (k >= kInterrupt) {
        const std::size_t stride =
            inline_stride(keys, values, count, kKinds[k].NameOffset);
        if (stride == 0) return false;
        return harvest_inline(keys, values, count, kKinds[k].NameOffset,
                              stride, into) > 0;
    }

    if (!self_consistent(keys, values, count, kKinds[k].NameOffset)) {
        return false;
    }
    // Which kind still has to be confirmed rather than taken from the key
    // the path was recorded under: the spell and status managers look
    // identical, and adopting one as the other would be silent.
    if (kind_of(keys, count) != (int)k) return false;
    return harvest(keys, values, count, kKinds[k].NameOffset, into) > 0;
}

// The managers a previous run recorded a path to, with no scanning.
//
// Only the kinds that run found are recorded, so this publishes the same
// set the scan would -- but if a later game state would expose a manager
// the recording run never saw, the cache has to be deleted for the scan
// to look again.
bool build_from_statics() {
    struct { Table Kinds[std::size(kKinds)]; } found{};
    std::size_t kinds = 0;

    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        const std::string key = static_key(k);
        const std::size_t count = bg3le_static_count(key.c_str());
        for (std::size_t i = 0; i < count; ++i) {
            void* at = bg3le_static_get(key.c_str(), i);
            if (at == nullptr) continue;
            if (!harvest_map((unsigned long long)(std::uintptr_t)at, k,
                             &found.Kinds[k])) {
                found.Kinds[k].ByName.clear();
                continue;
            }
            logf("prototypes: %zu %s prototypes at %p without scanning, "
                 "from recorded static %zu of %zu",
                 found.Kinds[k].ByName.size(), kKinds[k].Name, at, i, count);
            bg3le_static_confirm(key.c_str(), i);
            ++kinds;
            break;
        }
    }

    if (kinds == 0) return false;

    Prototypes& live = state();
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        Table& table = found.Kinds[k];
        table.Order.reserve(table.ByName.size());
        for (auto const& entry : table.ByName) {
            table.Order.push_back(entry.first);
        }
        live.Kinds[k] = std::move(table);
    }
    live.Built.store(true, std::memory_order_release);
    return true;
}

bool build() {
    struct { Table Kinds[std::size(kKinds)]; } found{};
    unsigned long long at[std::size(kKinds)] = {};
    std::vector<Candidate> candidates;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kChunk = 1u << 20;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kMapSize);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kMapSize) want = kChunk + kMapSize;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            scan_yield();
            if (got < kMapSize) continue;

            for (std::size_t off = 0; off + kMapSize <= got; off += 8) {
                // From the block, no syscalls: the counts have to be in
                // range before anything is dereferenced.
                std::uint32_t capacity = 0;
                std::uint32_t count = 0;
                std::memcpy(&capacity, block.data() + off + kKeysCapacity,
                            sizeof(capacity));
                std::memcpy(&count, block.data() + off + kKeysSize,
                            sizeof(count));
                if (count < kMinEntries || count > kMaxEntries
                    || count > capacity) {
                    continue;
                }

                // A real HashMap's three internal arrays agree with one
                // another, and that can be checked from the block without
                // a single read. It has to be: the count test alone left
                // tens of thousands of candidates, each costing two dozen
                // process_vm_readv calls, and that volume takes the mmap
                // lock often enough to leave the game blocked on its own
                // allocations -- idle rather than busy, and unable to
                // tick.
                std::uint64_t hashKeys = 0;
                std::uint32_t hashKeysSize = 0;
                std::uint64_t nextIds = 0;
                std::uint32_t nextIdsCapacity = 0;
                std::uint32_t nextIdsSize = 0;
                std::uint64_t keysWord = 0;
                std::uint64_t valuesWord = 0;
                std::memcpy(&hashKeys, block.data() + off, sizeof(hashKeys));
                std::memcpy(&hashKeysSize, block.data() + off + 8,
                            sizeof(hashKeysSize));
                std::memcpy(&nextIds, block.data() + off + 16,
                            sizeof(nextIds));
                std::memcpy(&nextIdsCapacity, block.data() + off + 24,
                            sizeof(nextIdsCapacity));
                std::memcpy(&nextIdsSize, block.data() + off + 28,
                            sizeof(nextIdsSize));
                std::memcpy(&keysWord, block.data() + off + kKeysBuffer,
                            sizeof(keysWord));
                std::memcpy(&valuesWord, block.data() + off + kValuesBuffer,
                            sizeof(valuesWord));

                // NextIds runs parallel to Keys, so their extents match.
                if (nextIdsSize != count || nextIdsCapacity != capacity) {
                    continue;
                }
                // The bucket table is sized for the entries it holds.
                if (hashKeysSize < count / 2 || hashKeysSize > count * 4) {
                    continue;
                }
                // And all four buffers are heap pointers.
                auto plausible = [](std::uint64_t p) {
                    return p > 0x10000 && p < 0x800000000000ull
                           && (p & 7) == 0;
                };
                if (!plausible(hashKeys) || !plausible(nextIds)
                    || !plausible(keysWord) || !plausible(valuesWord)) {
                    continue;
                }

                auto const* keys = (void const*)(std::uintptr_t)keysWord;
                auto const* values = (void const*)(std::uintptr_t)valuesWord;

                // Every prototype names itself at the same offset, so
                // one check decides whether this is a prototype map at
                // all. Which kind it is costs stat lookups, so it is
                // settled after the scan rather than inside it: asking
                // per candidate meant a linear walk of 15,754 stats for
                // each one, which starved the story thread until the
                // debugger could not get a tick.
                if (!self_consistent(keys, values, count,
                                     kKinds[kSpell].NameOffset)) {
                    continue;
                }
                if (candidates.size() < kMaxCandidates) {
                    candidates.push_back(Candidate{keys, values, count,
                                                   base + off});
                }
            }
        }
    }
    std::fclose(maps);

    // Now the stat lookups. Only the largest few candidates, because a
    // real manager holds thousands of entries and each lookup is a linear
    // walk of every stat -- asking all of them would be millions of reads
    // for an answer two maps decide.
    std::sort(candidates.begin(), candidates.end(),
              [](Candidate const& a, Candidate const& b) {
                  return a.Count > b.Count;
              });
    // Every candidate, before the pointer pass narrows to the largest
    // few: the passive and interrupt managers hold hundreds where the
    // spell and status ones hold thousands, so truncating first left them
    // out entirely. The inline test costs reads rather than stat lookups,
    // so it can afford the whole list.
    std::vector<Candidate> const inlineCandidates = candidates;
    if (candidates.size() > kClassify) candidates.resize(kClassify);

    for (Candidate const& candidate : candidates) {
        // Both found: stop. Classifying a leftover costs a linear walk of
        // every stat per key, and doing that for the remaining candidates
        // put over two minutes between finding the maps and publishing
        // them -- long enough that a caller in between saw nothing.
        if (!found.Kinds[kSpell].ByName.empty()
            && !found.Kinds[kStatus].ByName.empty()) {
            break;
        }

        const int kind = kind_of(candidate.Keys, candidate.Count);
        if (kind < 0) continue;
        if (!found.Kinds[kind].ByName.empty()) continue;

        const std::size_t added =
            harvest(candidate.Keys, candidate.Values, candidate.Count,
                    kKinds[kind].NameOffset, &found.Kinds[kind]);
        if (added > 0) {
            at[kind] = candidate.At;
            logf("prototypes: %zu %s prototypes at %#llx", added,
                 kKinds[kind].Name, candidate.At);
        }
    }

    // The inline-valued managers. Same candidates, different value
    // layout: the prototype sits in the map rather than behind a pointer,
    // so the stride is derived and the name still has to match the key.
    for (Candidate const& candidate : inlineCandidates) {
        for (std::size_t k = kInterrupt; k <= kPassive; ++k) {
            if (!found.Kinds[k].ByName.empty()) continue;

            const std::size_t stride = inline_stride(
                candidate.Keys, candidate.Values, candidate.Count,
                kKinds[k].NameOffset);
            if (stride == 0) continue;

            const std::size_t added = harvest_inline(
                candidate.Keys, candidate.Values, candidate.Count,
                kKinds[k].NameOffset, stride, &found.Kinds[k]);
            if (added > 0) {
                at[k] = candidate.At;
                logf("prototypes: %zu %s prototypes at %#llx, stride %zu",
                     added, kKinds[k].Name, candidate.At, stride);
            }
            break;
        }
    }

    std::size_t total = 0;
    for (auto const& table : found.Kinds) total += table.ByName.size();
    if (total == 0) {
        logf("prototypes: no prototype manager found; the cached prototype "
             "getters stay unavailable");
        return false;
    }

    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        Table& table = found.Kinds[k];
        table.Order.reserve(table.ByName.size());
        for (auto const& entry : table.ByName) {
            table.Order.push_back(entry.first);
        }
    }

    // Filled first, published second.
    Prototypes& live = state();
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        live.Kinds[k] = std::move(found.Kinds[k]);
    }
    live.Built.store(true, std::memory_order_release);

    // Recorded so the next run reads the engine's own pointer. The window
    // is one map: the scan lands on the map itself, and whatever owns it
    // points at the object the map is a member of.
    constexpr std::uint64_t kOwnerWindow = 1u << 12;
    for (std::size_t k = 0; k < std::size(kKinds); ++k) {
        if (at[k] == 0) continue;
        bg3le_static_record_path(static_key(k).c_str(),
                                 (void const*)(std::uintptr_t)at[k],
                                 kOwnerWindow);
    }

    logf("prototypes: %zu spells and %zu statuses available",
         live.Kinds[kSpell].ByName.size(),
         live.Kinds[kStatus].ByName.size());
    return true;
}

bool ready() {
    if (state().Built.load(std::memory_order_acquire)) return true;

    // Resolving a recorded pointer is not a scan, so any thread may do it.
    if (build_from_statics()) return true;

    // Only the warming thread scans; see mem.h.
    if (!scan_allowed()) return false;

    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return build();
}

Table const* table_of(int kind) {
    if (!ready() || kind < 0 || (std::size_t)kind >= std::size(kKinds)) {
        return nullptr;
    }
    return &state().Kinds[kind];
}

}  // namespace

extern "C" bool bg3le_prototypes_ready() { return ready(); }

// kind: 0 spell, 1 status.
extern "C" void* bg3le_prototype_find(int kind, char const* name) {
    Table const* table = table_of(kind);
    if (table == nullptr || name == nullptr) return nullptr;

    auto it = table->ByName.find(name);
    if (it == table->ByName.end()) return nullptr;
    return (void*)(std::uintptr_t)it->second;
}

extern "C" std::size_t bg3le_prototype_count(int kind) {
    Table const* table = table_of(kind);
    return table != nullptr ? table->ByName.size() : 0;
}

extern "C" char const* bg3le_prototype_name_at(int kind, std::size_t index) {
    Table const* table = table_of(kind);
    if (table == nullptr || index >= table->Order.size()) return nullptr;
    return table->Order[index].c_str();
}

}  // namespace bg3le
