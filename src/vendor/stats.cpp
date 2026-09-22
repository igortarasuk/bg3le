// Finds the engine's stats manager, which is what Ext.Stats reads.
//
// Unlike the GUID resource manager, RPGStats has no ECS type index to
// fingerprint against -- the symbol table only carries eoc::RPGStatsComponent
// and esv::RPGStatsSystem, which are the ECS component and system, not this.
// What it does have is FixedString TreasureRarities[7], seven consecutive
// string indices whose text is known: Common, Unique, Uncommon, Rare, Epic,
// Legendary, Divine, in the order ItemDataRarity declares them. Seven
// consecutive indices resolving to those seven names in that order is not a
// coincidence, and bg3le already has the string table needed to read them.
//
// The object base is then that address minus the offset of TreasureRarities,
// and the guess is checked rather than trusted: Objects.Values has to hold a
// plausible buffer and a plausible count before the address is accepted. This
// matters because struct offsets in this codebase are not automatically
// portable -- the Linux CRITICAL_SECTION shim already makes our FixedString
// sub-table 0x1208 where Windows has 0x1200 -- so an offset that happens to
// be wrong has to fail loudly rather than return garbage.
//
// Reading a stat's attributes takes four lookups, because the values are
// stored apart from their names:
//
//   Object.ModifierListIndex -> RPGStats.ModifierLists   -> ModifierList
//   ModifierList.Attributes.Values[n]                    -> Modifier
//   Object.IndexedProperties[n]                          -> the raw int32
//   Modifier.EnumerationIndex -> RPGStats.ModifierValueLists -> RPGEnumeration
//
// The enumeration says how to read the int: ConstantInt and ConstantFloat are
// literal, FixedString and Guid are indices into their tables, and anything
// with labels is an enumeration or a flag set. That mapping is name-based in
// upstream too (RPGEnumeration::GetPropertyType compares against known type
// names), so it is reproduced here by comparing the resolved text.
//
// The types are used as declared rather than walked by hand: bg3se's headers
// describe RPGStats, Object, ModifierList, Modifier and RPGEnumeration, and
// CoreLib carries LegacyMap, so once the base address is known every access
// is ordinary member access.

#include <stdafx.h>

#include <GameDefinitions/Stats/Stats.h>
#include <GameDefinitions/Components/All.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace {

using bg3se::stats::Modifier;
using bg3se::stats::ModifierList;
using bg3se::stats::Object;
using bg3se::stats::RPGEnumeration;
using bg3se::stats::RPGStats;

// ItemDataRarity's first seven values, in declaration order, which is the
// order TreasureRarities stores them in.
constexpr char const* kRarities[] = {"Common",    "Unique", "Uncommon",
                                     "Rare",      "Epic",   "Legendary",
                                     "Divine"};
constexpr std::size_t kRarityCount =
    sizeof(kRarities) / sizeof(kRarities[0]);

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// The text of a FixedString, or null. FixedString is a 32-bit index into the
// engine's global string table.
char const* text_of(bg3se::FixedString const& fs) {
    return bg3le_fixed_string(fs.Index, nullptr);
}

extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out);

// The seven rarity indices, looked up once by name.
//
// Knowing the values turns the scan into a search for one exact 32-bit
// integer followed by a 28-byte compare. Guessing at plausible indices
// instead -- seven arithmetic tests at every four-byte offset -- took 114
// seconds; this is a plain memory scan.
bool rarity_indices(std::uint32_t* out) {
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        if (!bg3le_fixed_string_index_of(kRarities[i], &out[i])) {
            logf("stats: the string table has no entry for \"%s\", so the "
                 "rarity fingerprint cannot be built; Ext.Stats stays "
                 "unavailable", kRarities[i]);
            return false;
        }
    }
    return true;
}

// Identifies the managers by what they contain, not by where the header says
// they are.
//
// The first attempt trusted offsetof: it found the single rarity run, took
// offsetof(RPGStats, TreasureRarities) off it, and validated the result. That
// was wrong twice over. The offset is 800 in our build and 3648 in the
// engine's, so the struct differs -- and once the offset is unknown, scanning
// 1024 candidate offsets against a loose test (three non-null pointers with
// any size under a million, one resolvable string) accepts a coincidence.
// It did: the search reported success and Objects.size read back as zero.
//
// So nothing here uses a member offset. A CNamedElementManager begins with
// Array<T*>, which is a pointer, a capacity and a size; the stats array is
// the one whose elements are stat objects, and that is checked by resolving
// many of their names rather than one. Ten consecutive resolvable names out
// of a candidate array is not a coincidence.
// Whether seven consecutive indices are the seven rarity indices in some
// order.
bool is_rarity_permutation(std::uint32_t const* v,
                           std::uint32_t const* want) {
    bool seen[kRarityCount] = {};
    for (std::size_t i = 0; i < kRarityCount; ++i) {
        bool matched = false;
        for (std::size_t k = 0; k < kRarityCount; ++k) {
            if (seen[k] || v[i] != want[k]) continue;
            seen[k] = true;
            matched = true;
            break;
        }
        if (!matched) return false;
    }
    return true;
}

struct ArrayRef {
    void const* Buffer{nullptr};
    std::uint32_t Size{0};
};

bool array_header_at(void const* at, ArrayRef* out) {
    std::uint32_t capacity = 0;
    if (!read_as((char const*)at + 0, &out->Buffer)) return false;
    if (!read_as((char const*)at + 8, &capacity)) return false;
    if (!read_as((char const*)at + 12, &out->Size)) return false;
    if (out->Buffer == nullptr) return false;
    if (out->Size == 0 || out->Size > capacity) return false;
    if (capacity > 4000000) return false;
    return true;
}

// How many of the first n elements are pointers to something with a
// resolvable, *distinct* FixedString at the given offset.
//
// Distinctness is the whole test. Without it this accepted an array of
// visual resources whose first twelve entries all resolved to
// "DEC_HAR_Fish_Small_A...Mesh_LOD1.1" -- the same pointer repeated -- and
// reported 15754 stats that were nothing of the kind. Any array of pointers
// to named objects passes "the names resolve"; stat names are unique, so
// requiring them to differ rejects the rest.
std::size_t named_elements(ArrayRef const& array, std::size_t nameOffset,
                           std::size_t n) {
    std::uint32_t seen[32];
    std::size_t ok = 0;
    for (std::size_t i = 0; i < n && i < array.Size && ok < 32; ++i) {
        void const* element = nullptr;
        if (!read_as((char const*)array.Buffer + i * sizeof(void*),
                     &element)) {
            break;
        }
        if (element == nullptr) break;
        bg3se::FixedString name{};
        if (!read_as((char const*)element + nameOffset, &name)) break;
        if (text_of(name) == nullptr) break;

        for (std::size_t k = 0; k < ok; ++k) {
            if (seen[k] == name.Index) return ok;   // a repeat: not stats
        }
        seen[ok++] = name.Index;
    }
    return ok;
}

// The stats array, found by content within a window around the rarity run.
//
// Object's own layout is as uncertain as RPGStats', so the offset of its Name
// is searched for too: whichever offset makes ten consecutive elements
// resolve is the right one, and it is logged so the drift is on record.
bool find_objects(unsigned long long runAddr, ArrayRef* out,
                  std::size_t* nameOffsetOut) {
    constexpr std::size_t kWindow = 16384;   // either side of the run
    constexpr std::size_t kProbe = 10;       // elements that must resolve
    constexpr std::size_t kMaxNameOffset = 128;

    const unsigned long long lo =
        runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    // Diagnostic: what array-like headers are actually near the run. Logged
    // because guessing which constraint is too strict wastes a run each time.
    std::size_t headers = 0;
    std::size_t biggest = 0;
    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef probe{};
        if (array_header_at((void const*)at, &probe)) {
            ++headers;
            if (probe.Size > biggest) biggest = probe.Size;
        }
    }
    logf("stats: %zu array-like headers within %zu bytes of the run, largest "
         "%zu entries", headers, (std::size_t)kWindow, biggest);

    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef array{};
        if (!array_header_at((void const*)at, &array)) continue;
        // BG3 ships thousands of stats; a smaller array is something else.
        if (array.Size < 1000) continue;

        for (std::size_t nameOff = 0; nameOff <= kMaxNameOffset;
             nameOff += 4) {
            if (named_elements(array, nameOff, kProbe) < kProbe) continue;
            *out = array;
            *nameOffsetOut = nameOff;
            logf("stats: stats array at %#llx, %u entries, Object::Name at "
                 "+%zu (header says +%zu)", at, array.Size, nameOff,
                 (std::size_t)offsetof(Object, Name));
            return true;
        }
    }
    return false;
}

// What the search establishes. No struct offsets survive into this.
struct Found {
    bool Searched{false};
    ArrayRef Objects{};
    std::size_t NameOffset{0};
};

Found& state() {
    static Found f;
    return f;
}

bool search_for_stats() {
    if (bg3le_fixed_string(1, nullptr) == nullptr) {
        logf("stats: the string table is not available, so the rarity "
             "fingerprint cannot be read; Ext.Stats stays unavailable");
        return false;
    }

    std::uint32_t want[kRarityCount] = {};
    if (!rarity_indices(want)) return false;

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kRunBytes = kRarityCount * sizeof(std::uint32_t);
    constexpr std::size_t kChunk = 1u << 20;

    static std::vector<unsigned char> block;
    block.resize(kChunk + kRunBytes);

    char line[512];
    std::size_t regions = 0;
    std::size_t scanned = 0;
    std::size_t hits = 0;

    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;
        ++regions;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t span = (std::size_t)(to - base);
            if (span > kChunk + kRunBytes) span = kChunk + kRunBytes;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), span);
            if (got < kRunBytes) continue;
            scanned += got;

            const std::size_t last = got - kRunBytes;
            for (std::size_t off = 0; off <= last; off += 4) {
                auto const* v = (std::uint32_t const*)(block.data() + off);
                // Any ordering, not the enum's.
                //
                // Requiring declaration order found a run reliably -- and it
                // was the ItemDataRarity label table, which stores them in
                // exactly that order. Around it sat 173 arrays whose largest
                // held 101 entries, nothing like the thousands of stats.
                // TreasureRarities has no reason to use the enum's order, so
                // the test is now set membership.
                if (!is_rarity_permutation(v, want)) continue;
                ++hits;

                ArrayRef objects{};
                std::size_t nameOffset = 0;
                if (!find_objects(base + off, &objects, &nameOffset)) continue;

                std::fclose(maps);
                state().Objects = objects;
                state().NameOffset = nameOffset;
                logf("stats: %u stats found via the rarity run at %#llx "
                     "(scanned %zu bytes over %zu regions)", objects.Size,
                     base + off, scanned, regions);
                return true;
            }
        }
    }

    std::fclose(maps);
    if (hits != 0) {
        logf("stats: found %zu rarity runs but no stats array near any of "
             "them; Ext.Stats stays unavailable", hits);
    } else {
        logf("stats: no rarity run found (scanned %zu bytes over %zu "
             "regions); the stats may not be parsed yet", scanned, regions);
    }
    return false;
}

// Caches success permanently and failure only briefly.
//
// Caching a failure forever would be wrong: the stats are parsed during load,
// so a search that runs before that finishes fails for a reason that stops
// being true -- the first attempt in practice fails on "Epic" not yet being
// in the string table. Retrying without a cooldown would be worse, since
// every call would rescan memory.
bool ready() {
    Found& f = state();
    if (f.Objects.Buffer != nullptr) return true;

    static std::time_t lastAttempt = 0;
    const std::time_t now = std::time(nullptr);
    if (lastAttempt != 0 && now - lastAttempt < 10) return false;
    lastAttempt = now;

    const auto started = std::clock();
    const bool ok = search_for_stats();
    const double ms =
        1000.0 * (double)(std::clock() - started) / (double)CLOCKS_PER_SEC;
    logf("stats: search took %.0f ms", ms);
    return ok;
}

void const* object_at(std::size_t index) {
    if (!ready()) return nullptr;
    Found const& f = state();
    if (index >= f.Objects.Size) return nullptr;
    void const* element = nullptr;
    if (!read_as((char const*)f.Objects.Buffer + index * sizeof(void*),
                 &element)) {
        return nullptr;
    }
    return element;
}

}  // namespace

// ---- the C surface Ext.Stats is built on ----

extern "C" void* bg3le_stats_manager() {
    return ready() ? (void*)state().Objects.Buffer : nullptr;
}

extern "C" std::size_t bg3le_stats_count() {
    return ready() ? state().Objects.Size : 0;
}

extern "C" void* bg3le_stats_at(std::size_t index) {
    return (void*)object_at(index);
}

extern "C" char const* bg3le_stats_name(void const* object) {
    if (object == nullptr || !ready()) return nullptr;
    bg3se::FixedString name{};
    if (!read_as((char const*)object + state().NameOffset, &name)) {
        return nullptr;
    }
    return text_of(name);
}

// Linear, like the resource bank lookup: the engine's hash for a FixedString
// key is its own, and a wrong hash misses silently where a scan either finds
// the name or does not.
extern "C" void* bg3le_stats_find(char const* wanted) {
    if (wanted == nullptr || !ready()) return nullptr;
    const std::size_t n = state().Objects.Size;
    for (std::size_t i = 0; i < n; ++i) {
        void const* obj = object_at(i);
        if (obj == nullptr) continue;
        char const* name = bg3le_stats_name(obj);
        if (name != nullptr && std::strcmp(name, wanted) == 0) {
            return (void*)obj;
        }
    }
    return nullptr;
}

// Attributes are not available yet.
//
// Reading them needs RPGStats::ModifierLists and ModifierValueLists, and the
// struct offsets that would reach them are the very thing that proved wrong:
// our TreasureRarities sits at 800 where the engine's is at 3648. They have
// to be found by content, the way the stats array now is, and until that is
// done these report nothing rather than returning numbers read from the
// wrong place.
extern "C" char const* bg3le_stats_type(void const*) { return nullptr; }

extern "C" std::size_t bg3le_stats_attr_count(void const*) { return 0; }

extern "C" bool bg3le_stats_attr_at(void const*, std::size_t, char const**,
                                    char const**, int*, int*) {
    return false;
}

extern "C" char const* bg3le_stats_attr_label(void const*, std::size_t, int) {
    return nullptr;
}

extern "C" char const* bg3le_stats_attr_string(int raw) {
    if (raw < 0) return nullptr;
    return bg3le_fixed_string((std::uint32_t)raw, nullptr);
}

}  // namespace bg3le
