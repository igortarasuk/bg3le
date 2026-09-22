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
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);

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

// What the search establishes. No struct offsets survive into this: every
// one of these was derived by looking at the memory, because RPGStats' own
// offsets are wrong for this build (TreasureRarities at 800 here, 3648 in the
// engine).
struct Found {
    ArrayRef Objects{};
    std::size_t NameOffset{0};          // Object::Name

    ArrayRef Lists{};                   // RPGStats::ModifierLists
    std::size_t ListNameOffset{0};      // ModifierList::Name
    std::size_t ModifierNameOffset{0};  // Modifier::Name
    std::size_t AttrsOffset{0};         // ModifierList::Attributes

    ArrayRef ValueLists{};              // RPGStats::ModifierValueLists
    std::size_t ValueNameOffset{0};     // RPGEnumeration::Name

    std::size_t PropsOffset{0};         // Object::IndexedProperties
    std::size_t ListIndexOffset{0};     // Object::ModifierListIndex
    ArrayRef Strings{};                 // RPGStats::FixedStrings
    ArrayRef Floats{};                  // RPGStats::Floats
    ArrayRef Guids{};                   // RPGStats::GUIDs
    bool Attributes{false};             // whether all of the above landed
};

Found& state() {
    static Found f;
    return f;
}

// A pointer array containing an element with a specific name.
//
// This is the test that cannot be faked. The modifier value lists must
// contain an entry called "ConstantInt", because that is the type name the
// engine compares against when deciding how to read an attribute; the
// modifier lists must contain one called "Weapon". An array of unrelated
// named objects will not.
bool array_contains_name(ArrayRef const& array, std::size_t nameOffset,
                         char const* wanted, std::size_t limit) {
    for (std::size_t i = 0; i < array.Size && i < limit; ++i) {
        void const* element = nullptr;
        if (!read_as((char const*)array.Buffer + i * sizeof(void*),
                     &element)) {
            return false;
        }
        if (element == nullptr) continue;
        bg3se::FixedString name{};
        if (!read_as((char const*)element + nameOffset, &name)) continue;
        char const* text = text_of(name);
        if (text != nullptr && std::strcmp(text, wanted) == 0) return true;
    }
    return false;
}

// Finds a pointer array near the run whose elements carry a given name at
// some offset, identified by a name it must contain.
bool find_named_array(unsigned long long runAddr, char const* mustContain,
                      std::size_t minSize, std::size_t maxSize,
                      std::size_t maxNameOffset, ArrayRef* out,
                      std::size_t* nameOffsetOut, char const* what) {
    constexpr std::size_t kWindow = 16384;
    const unsigned long long lo = runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    for (unsigned long long at = lo; at + 16 <= hi; at += 8) {
        ArrayRef array{};
        if (!array_header_at((void const*)at, &array)) continue;
        if (array.Size < minSize || array.Size > maxSize) continue;

        for (std::size_t nameOff = 0; nameOff <= maxNameOffset;
             nameOff += 4) {
            // Distinct names first, for the same reason as the stats array.
            if (named_elements(array, nameOff, 8) < 8) continue;
            if (!array_contains_name(array, nameOff, mustContain,
                                     array.Size)) {
                continue;
            }
            *out = array;
            *nameOffsetOut = nameOff;
            logf("stats: %s at %#llx, %u entries, name at +%zu (contains "
                 "\"%s\")", what, at, array.Size, nameOff, mustContain);
            return true;
        }
    }
    logf("stats: no %s near the run (wanted an array of %zu-%zu entries "
         "containing \"%s\")", what, minSize, maxSize, mustContain);
    return false;
}

// Modifier::Name, found by requiring a list's attributes to have distinct
// resolvable names.
bool find_modifier_name_offset(ArrayRef const& lists, std::size_t* out,
                               std::size_t* attrsOffsetOut) {
    // The attribute array's offset is searched for, not assumed. Assuming
    // zero failed: a ModifierList starts with a vtable pointer, which our
    // header does not declare, so the array actually begins at +8. The
    // reconstructed layout agrees with the name landing at +92 --
    // VMT(8) + Array(16) + HashMap(64) + int32(4).
    for (std::size_t i = 0; i < lists.Size && i < 16; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)lists.Buffer + i * sizeof(void*), &list)) {
            continue;
        }
        if (list == nullptr) continue;

        for (std::size_t attrsOff = 0; attrsOff <= 32; attrsOff += 8) {
            ArrayRef attrs{};
            if (!array_header_at((char const*)list + attrsOff, &attrs)) {
                continue;
            }
            if (attrs.Size < 8) continue;

            for (std::size_t off = 0; off <= 64; off += 4) {
                if (named_elements(attrs, off, 8) < 8) continue;
                *out = off;
                *attrsOffsetOut = attrsOff;
                logf("stats: Modifier::Name at +%zu, ModifierList::Attributes "
                     "at +%zu (%u attributes on list %zu)", off, attrsOff,
                     attrs.Size, i);
                return true;
            }
        }
    }
    // Diagnostic: what the first list's attribute array actually looks like.
    for (std::size_t i = 0; i < lists.Size && i < 2; ++i) {
        void const* list = nullptr;
        if (!read_as((char const*)lists.Buffer + i * sizeof(void*), &list)
            || list == nullptr) {
            continue;
        }
        ArrayRef attrs{};
        const bool header = array_header_at(list, &attrs);
        logf("stats:   list %zu at %p: header=%d size=%u buffer=%p", i, list,
             header ? 1 : 0, header ? attrs.Size : 0,
             header ? attrs.Buffer : nullptr);
        if (!header) {
            std::uint64_t words[4] = {};
            if (read_as(list, &words)) {
                logf("stats:     first words %#lx %#lx %#lx %#lx", words[0],
                     words[1], words[2], words[3]);
            }
            continue;
        }
        void const* first = nullptr;
        if (read_as(attrs.Buffer, &first) && first != nullptr) {
            std::uint32_t w[8] = {};
            if (read_as(first, &w)) {
                logf("stats:     modifier[0] at %p: %u %u %u %u %u %u %u %u",
                     first, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }
    }
    logf("stats: could not place Modifier::Name; attributes stay unavailable");
    return false;
}

// Strings the value pool must contain, and a list of names cannot.
constexpr char const* kDiceStrings[] = {"1d8", "1d6", "1d10", "2d6"};
constexpr std::size_t kDiceCount =
    sizeof(kDiceStrings) / sizeof(kDiceStrings[0]);

// Whether a pool of FixedString indices holds any of the given strings.
bool pool_contains_any(ArrayRef const& pool, char const* const* wanted,
                       std::size_t count) {
    std::uint32_t ids[8];
    std::size_t n = 0;
    for (std::size_t i = 0; i < count && n < 8; ++i) {
        std::uint32_t id = 0;
        if (bg3le_fixed_string_index_of(wanted[i], &id)) ids[n++] = id;
    }
    if (n == 0) {
        static bool said = false;
        if (!said) {
            said = true;
            logf("stats: none of the dice strings are in the string table, so "
                 "the value pool cannot be identified that way");
        }
        return false;
    }

    const std::size_t limit = pool.Size < 200000 ? pool.Size : 200000;
    for (std::size_t i = 0; i < limit; ++i) {
        std::uint32_t entry = 0;
        if (!read_as((char const*)pool.Buffer + i * sizeof(std::uint32_t),
                     &entry)) {
            return false;
        }
        for (std::size_t k = 0; k < n; ++k) {
            if (entry == ids[k]) return true;
        }
    }
    return false;
}

// RPGStats::FixedStrings, the pool a FixedString attribute indexes into.
//
// An attribute's raw int is not a global string index. It is a position in
// this pool, which is why "Damage" read back as 2303 -- decoded as a string
// index its sub-table nibble is 15, and only 11 exist -- and why every unset
// attribute resolved to "Version64", the string at index 0.
//
// The header puts FixedStrings immediately after TreasureRarities, so the
// rarity run locates it: a CompactSet of uint32 string indices that resolve.
bool find_string_pool(unsigned long long runAddr, ArrayRef* out,
                      unsigned long long* poolAddrOut) {
    // Both directions, and wide. Searching only forwards found nothing: the
    // header has FixedStrings just after TreasureRarities, but the engine's
    // member order plainly differs -- the modifier lists sit *below* the run
    // in memory, not above it.
    constexpr std::size_t kWindow = 16384;
    constexpr std::size_t kProbe = 8;
    const unsigned long long lo = runAddr > kWindow ? runAddr - kWindow : 0;
    const unsigned long long hi = runAddr + kWindow;

    for (unsigned long long at = lo; at + 16 <= hi; at += 4) {
        ArrayRef pool{};
        if (!array_header_at((void const*)at, &pool)) continue;
        // The pool holds every string the stats files mention, so it is
        // large; a small array of resolvable indices is something else.
        if (pool.Size < 256) continue;

        // No structural pre-filter. Requiring the first entries to resolve
        // and differ rejected the real pool on its first run -- a pool may
        // hold empty or repeated slots -- while happily accepting the stat
        // name array, which has neither. The dice test below is the only
        // thing that actually distinguishes them, so it decides alone.

        // Distinctness is not enough here. The Objects manager keeps a
        // NameToHandle map whose key array is 15754 resolvable, distinct
        // string indices -- the stat names -- and matching it made every
        // attribute read back as a stat name ("Damage" came out as
        // "Interrupt_BardicInspiration_SavingThrow_d8", unset ones as
        // "Target_MainHandAttack", which is stat zero).
        //
        // So the test is positive rather than structural: the value pool has
        // to contain dice notation, because that is what weapon damage is
        // written as. A list of stat names does not.
        if (!pool_contains_any(pool, kDiceStrings, kDiceCount)) {
            logf("stats:   candidate pool at %#llx rejected: %u resolvable "
                 "entries but no dice notation", at, pool.Size);
            continue;
        }

        *out = pool;
        *poolAddrOut = at;
        logf("stats: string pool at %#llx (%+lld from the run), %u entries",
             at, (long long)(at - runAddr), pool.Size);
        return true;
    }
    logf("stats: no string pool found after the rarity run; FixedString "
         "attributes will report their raw index");
    return false;
}

// RPGStats::Floats and RPGStats::GUIDs, the pools the other indexed
// attribute kinds point into.
//
// The header's member order held for the string pool -- it landed exactly
// where TreasureRarities plus its padding predicted -- so the same order is
// used here: FixedStrings, Int64s, GUIDs, Floats. Each is a 16-byte array
// header, so the candidates are a short walk forward, and each is checked
// against what its contents should look like rather than accepted on
// position alone.
void find_value_pools(unsigned long long poolAddr, Found* f) {
    // Floats: finite, and not a block of zeroes or garbage exponents.
    auto plausible_floats = [](ArrayRef const& a) {
        if (a.Size < 8) return false;
        std::size_t sane = 0;
        std::size_t nonzero = 0;
        for (std::size_t i = 0; i < 16 && i < a.Size; ++i) {
            float v = 0.0f;
            if (!read_as((char const*)a.Buffer + i * sizeof(float), &v)) {
                return false;
            }
            const float mag = v < 0 ? -v : v;
            if (v == v && mag < 1e9f) ++sane;      // v == v rejects NaN
            if (v != 0.0f) ++nonzero;
        }
        return sane >= 16 && nonzero >= 2;
    };

    // Guids: 16 bytes each, and a real one is not all zeroes.
    auto plausible_guids = [](ArrayRef const& a) {
        if (a.Size < 4) return false;
        std::size_t nonzero = 0;
        for (std::size_t i = 0; i < 8 && i < a.Size; ++i) {
            std::uint64_t w[2] = {};
            if (!read_as((char const*)a.Buffer + i * 16, &w)) return false;
            if (w[0] != 0 || w[1] != 0) ++nonzero;
        }
        return nonzero >= 4;
    };

    // At the positions the member order predicts, not the first thing that
    // passes. Scanning forward for "the first plausible guid array" picked
    // pool+16, which is Int64s: an array of pointers read sixteen bytes at a
    // time looks exactly like non-zero guids. The order is
    // FixedStrings, Int64s, GUIDs, Floats, and floats landing at +48 on the
    // first run is what confirms it.
    constexpr std::size_t kGuidsAt = 32;
    constexpr std::size_t kFloatsAt = 48;

    ArrayRef guids{};
    if (array_header_at((void const*)(poolAddr + kGuidsAt), &guids)
        && plausible_guids(guids)) {
        f->Guids = guids;
        logf("stats: guid pool at pool+%zu, %u entries", kGuidsAt,
             guids.Size);
    }

    ArrayRef floats{};
    if (array_header_at((void const*)(poolAddr + kFloatsAt), &floats)
        && plausible_floats(floats)) {
        f->Floats = floats;
        logf("stats: float pool at pool+%zu, %u entries", kFloatsAt,
             floats.Size);
    }
    if (f->Floats.Buffer == nullptr) {
        logf("stats: no float pool found; Float attributes report their "
             "pool index");
    }
    if (f->Guids.Buffer == nullptr) {
        logf("stats: no guid pool found; GUID attributes report their "
             "pool index");
    }
}

// Object::IndexedProperties and Object::ModifierListIndex, derived from the
// one relationship that has to hold: an object's value count equals the
// number of attributes in its modifier list.
//
// This is self-validating, which matters because these two offsets sit after
// members whose size cannot be confirmed from the headers. A pair of offsets
// that agrees across many objects is right; nothing else would.
bool find_object_offsets(Found const& f, std::size_t* propsOut,
                         std::size_t* indexOut) {
    constexpr std::size_t kSamples = 24;
    constexpr std::size_t kMaxProps = 64;
    constexpr std::size_t kMaxIndex = 512;

    // Vector<int32_t> is a begin/end pair, so the count is the byte span
    // divided by four.
    auto vector_count = [](void const* at, std::size_t* countOut) {
        void const* begin = nullptr;
        void const* end = nullptr;
        if (!read_as((char const*)at + 0, &begin)) return false;
        if (!read_as((char const*)at + 8, &end)) return false;
        if (begin == nullptr || end < begin) return false;
        const std::size_t bytes =
            (std::size_t)((char const*)end - (char const*)begin);
        if (bytes % 4 != 0 || bytes > (1u << 20)) return false;
        *countOut = bytes / 4;
        return true;
    };

    auto list_attr_count = [&f](std::uint32_t listIndex,
                                std::size_t* countOut) {
        if (listIndex >= f.Lists.Size) return false;
        void const* list = nullptr;
        if (!read_as((char const*)f.Lists.Buffer + listIndex * sizeof(void*),
                     &list)) {
            return false;
        }
        if (list == nullptr) return false;
        ArrayRef attrs{};
        if (!array_header_at((char const*)list + f.AttrsOffset, &attrs)) {
            return false;
        }
        *countOut = attrs.Size;
        return true;
    };

    for (std::size_t props = 0; props <= kMaxProps; props += 8) {
        for (std::size_t idx = 0; idx <= kMaxIndex; idx += 4) {
            std::size_t agreed = 0;
            std::size_t tried = 0;

            for (std::size_t i = 0; i < f.Objects.Size && tried < kSamples;
                 ++i) {
                void const* obj = nullptr;
                if (!read_as((char const*)f.Objects.Buffer + i * sizeof(void*),
                             &obj)) {
                    break;
                }
                if (obj == nullptr) continue;
                ++tried;

                std::size_t valueCount = 0;
                if (!vector_count((char const*)obj + props, &valueCount)) {
                    break;
                }
                std::uint32_t listIndex = 0;
                if (!read_as((char const*)obj + idx, &listIndex)) break;

                std::size_t attrCount = 0;
                if (!list_attr_count(listIndex, &attrCount)) break;
                if (valueCount != attrCount || valueCount == 0) break;
                ++agreed;
            }

            if (tried >= kSamples && agreed == tried) {
                *propsOut = props;
                *indexOut = idx;
                logf("stats: Object::IndexedProperties at +%zu, "
                     "ModifierListIndex at +%zu (agreed on %zu objects)",
                     props, idx, agreed);
                return true;
            }
        }
    }
    logf("stats: no offsets made an object's value count match its modifier "
         "list's attribute count; attributes stay unavailable");
    return false;
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
                Found& f = state();
                f.Objects = objects;
                f.NameOffset = nameOffset;
                logf("stats: %u stats found via the rarity run at %#llx "
                     "(scanned %zu bytes over %zu regions)", objects.Size,
                     base + off, scanned, regions);

                // Attributes need three more things, each identified by
                // content. Any of them missing leaves enumeration working
                // and attributes reporting themselves unavailable.
                const unsigned long long run = base + off;
                const bool lists = find_named_array(
                    run, "Weapon", 4, 4096, 128, &f.Lists, &f.ListNameOffset,
                    "modifier lists");
                unsigned long long poolAddr = 0;
                if (find_string_pool(run, &f.Strings, &poolAddr)) {
                    find_value_pools(poolAddr, &f);
                }
                const bool values = find_named_array(
                    run, "ConstantInt", 4, 65536, 32, &f.ValueLists,
                    &f.ValueNameOffset, "modifier value lists");
                bool offsets = false;
                if (lists) {
                    offsets = find_modifier_name_offset(f.Lists, &f.ModifierNameOffset,
                                                        &f.AttrsOffset)
                              && find_object_offsets(f, &f.PropsOffset,
                                                     &f.ListIndexOffset);
                }
                f.Attributes = lists && values && offsets;
                logf("stats: attributes %s",
                     f.Attributes ? "available" : "unavailable");
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

// ---- attributes ----
//
// Four lookups, because the values are stored apart from their names:
//
//   Object[ListIndexOffset]        -> index into ModifierLists
//   ModifierList.Attributes[n]     -> Modifier, which carries the name
//   Object.IndexedProperties[n]    -> the raw int32
//   Modifier.EnumerationIndex      -> RPGEnumeration, which says how to read
//
// Every offset here was derived by looking at memory, not taken from the
// headers; see the search above for why.

namespace {

void const* list_for(void const* object) {
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return nullptr;

    std::uint32_t index = 0;
    if (!read_as((char const*)object + f.ListIndexOffset, &index)) {
        return nullptr;
    }
    if (index >= f.Lists.Size) return nullptr;

    void const* list = nullptr;
    if (!read_as((char const*)f.Lists.Buffer + index * sizeof(void*),
                 &list)) {
        return nullptr;
    }
    return list;
}

void const* modifier_at(void const* object, std::size_t index) {
    void const* list = list_for(object);
    if (list == nullptr) return nullptr;
    ArrayRef attrs{};
    if (!array_header_at((char const*)list + state().AttrsOffset, &attrs)) {
        return nullptr;
    }
    if (index >= attrs.Size) return nullptr;
    void const* mod = nullptr;
    if (!read_as((char const*)attrs.Buffer + index * sizeof(void*), &mod)) {
        return nullptr;
    }
    return mod;
}

// The enumeration a modifier's value should be read through. EnumerationIndex
// is the modifier's first member.
void const* enumeration_for(void const* modifier) {
    Found const& f = state();
    if (modifier == nullptr || f.ValueLists.Buffer == nullptr) return nullptr;
    std::int32_t index = 0;
    if (!read_as(modifier, &index)) return nullptr;
    if (index < 0 || (std::size_t)index >= f.ValueLists.Size) return nullptr;
    void const* en = nullptr;
    if (!read_as((char const*)f.ValueLists.Buffer + (std::size_t)index
                     * sizeof(void*), &en)) {
        return nullptr;
    }
    return en;
}

// Upstream decides this by comparing the enumeration's name against known
// type names, so the same comparison is made here on the resolved text.
// Values are RPGEnumerationType in bg3se's declaration order.
int property_type(void const* enumeration) {
    if (enumeration == nullptr) return 13;                       // Unknown
    bg3se::FixedString name{};
    if (!read_as((char const*)enumeration + state().ValueNameOffset, &name)) {
        return 13;
    }
    char const* text = text_of(name);
    if (text == nullptr) return 13;

    if (std::strcmp(text, "ConstantInt") == 0) return 0;         // Int
    if (std::strcmp(text, "ConstantFloat") == 0) return 2;       // Float
    if (std::strcmp(text, "FixedString") == 0
        || std::strcmp(text, "StatusIDs") == 0) return 3;        // FixedString
    if (std::strcmp(text, "Guid") == 0) return 6;                // GUID
    if (std::strcmp(text, "StatsFunctors") == 0) return 7;
    if (std::strcmp(text, "Conditions") == 0
        || std::strcmp(text, "TargetConditions") == 0
        || std::strcmp(text, "UseConditions") == 0) return 8;
    if (std::strcmp(text, "RollConditions") == 0) return 9;
    if (std::strcmp(text, "Requirements") == 0) return 10;
    if (std::strcmp(text, "MemorizationRequirements") == 0) return 11;
    if (std::strcmp(text, "TranslatedString") == 0) return 12;
    return 4;                                                    // Enumeration
}

}  // namespace

extern "C" char const* bg3le_stats_type(void const* object) {
    void const* list = list_for(object);
    if (list == nullptr) return nullptr;
    bg3se::FixedString name{};
    if (!read_as((char const*)list + state().ListNameOffset, &name)) {
        return nullptr;
    }
    return text_of(name);
}

extern "C" std::size_t bg3le_stats_attr_count(void const* object) {
    void const* list = list_for(object);
    if (list == nullptr) return 0;
    ArrayRef attrs{};
    if (!array_header_at((char const*)list + state().AttrsOffset, &attrs)) {
        return 0;
    }
    return attrs.Size;
}

extern "C" bool bg3le_stats_attr_at(void const* object, std::size_t index,
                                    char const** nameOut,
                                    char const** typeNameOut, int* kindOut,
                                    int* rawOut) {
    Found const& f = state();
    if (!f.Attributes || object == nullptr) return false;

    void const* mod = modifier_at(object, index);
    if (mod == nullptr) return false;

    // The attribute's position is its index into the object's values.
    void const* begin = nullptr;
    void const* end = nullptr;
    auto const* props = (char const*)object + f.PropsOffset;
    if (!read_as(props + 0, &begin)) return false;
    if (!read_as(props + 8, &end)) return false;
    if (begin == nullptr || end < begin) return false;
    const std::size_t count =
        (std::size_t)((char const*)end - (char const*)begin) / 4;
    if (index >= count) return false;

    std::int32_t raw = 0;
    if (!read_as((char const*)begin + index * sizeof(std::int32_t), &raw)) {
        return false;
    }

    bg3se::FixedString modName{};
    if (!read_as((char const*)mod + f.ModifierNameOffset, &modName)) {
        return false;
    }

    void const* en = enumeration_for(mod);
    if (nameOut != nullptr) *nameOut = text_of(modName);
    if (typeNameOut != nullptr) {
        if (en != nullptr) {
            bg3se::FixedString enName{};
            *typeNameOut =
                read_as((char const*)en + f.ValueNameOffset, &enName)
                    ? text_of(enName)
                    : nullptr;
        } else {
            *typeNameOut = nullptr;
        }
    }
    if (kindOut != nullptr) *kindOut = property_type(en);
    if (rawOut != nullptr) *rawOut = raw;
    return true;
}

// For an enumeration-typed attribute, the label matching the raw value. The
// enumeration maps label to value, so this is a reverse lookup over it.
extern "C" char const* bg3le_stats_attr_label(void const* object,
                                              std::size_t index, int raw) {
    void const* mod = modifier_at(object, index);
    void const* en = enumeration_for(mod);
    if (en == nullptr) return nullptr;

    // RPGEnumeration is Name then Values, so the map follows the name.
    auto const* map = (bg3se::LegacyMap<bg3se::FixedString, std::int32_t> const*)
        ((char const*)en + state().ValueNameOffset + 8);
    for (auto const& pair : *map) {
        if (pair.Value == raw) return text_of(pair.Key);
    }
    return nullptr;
}

// A Float attribute's value, from the float pool.
extern "C" bool bg3le_stats_attr_float(int raw, double* out) {
    Found const& f = state();
    if (raw < 0 || f.Floats.Buffer == nullptr) return false;
    if ((std::size_t)raw >= f.Floats.Size) return false;
    float v = 0.0f;
    if (!read_as((char const*)f.Floats.Buffer + (std::size_t)raw
                     * sizeof(float), &v)) {
        return false;
    }
    if (out != nullptr) *out = (double)v;
    return true;
}

// A GUID attribute's value, formatted the way the engine writes one.
extern "C" bool bg3le_stats_attr_guid(int raw, char* out,
                                      std::size_t capacity) {
    Found const& f = state();
    if (raw < 0 || f.Guids.Buffer == nullptr) return false;
    if ((std::size_t)raw >= f.Guids.Size) return false;
    std::uint8_t bytes[16] = {};
    if (!read_as((char const*)f.Guids.Buffer + (std::size_t)raw * 16,
                 &bytes)) {
        return false;
    }
    return bg3le_meta_format_guid(bytes, out, capacity);
}

// A FixedString attribute's text: the raw value indexes RPGStats' own pool,
// not the global string table.
extern "C" char const* bg3le_stats_attr_string(int raw) {
    Found const& f = state();
    if (raw < 0 || f.Strings.Buffer == nullptr) return nullptr;
    if ((std::size_t)raw >= f.Strings.Size) return nullptr;

    std::uint32_t id = 0;
    if (!read_as((char const*)f.Strings.Buffer
                     + (std::size_t)raw * sizeof(std::uint32_t), &id)) {
        return nullptr;
    }
    return bg3le_fixed_string(id, nullptr);
}

}  // namespace bg3le
