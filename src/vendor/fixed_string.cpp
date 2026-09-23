// Resolves a FixedString to its text, by finding the engine's global string
// table in memory.
//
// A FixedString is a 32-bit index into a table the engine builds at startup.
// Everything else in bg3le reads component data through layout that the
// compiler works out, but this one needs an address, and there is nothing to
// look up: ls::FixedString's methods are all inlined in the native build, so
// unlike the allocator there is no entry point to borrow, and the table itself
// has no symbol. It is a heap allocation reached through an anonymous global.
//
// It can be found by its shape instead. The table is an array of sub-tables,
// each carrying its own index, so eleven consecutive integers running 0..10 at
// a fixed stride identify it -- a coincidence of eleven ascending values at
// exactly the right spacing is not something a heap produces by accident. The
// candidate is then checked by resolving an entry and confirming the string's
// recorded length matches the bytes actually there.
//
// The layout and the index arithmetic below are bg3se's, from
// CoreLib/Base/BaseString.{h,inl} -- FixedStringBase::FindEntry. The walk is
// reimplemented here rather than called because FindEntry is protected and,
// more importantly, because it strides by sizeof(SubTable) as this build
// computes it, which is not the engine's: SubTable ends with a
// CRITICAL_SECTION, and the Linux stand-in for that is 48 bytes where the
// Windows type is 40. Every offset this needs sits before that member, so the
// offsets are reliable and only the stride is not -- and the stride is checked
// against the table rather than assumed.
//
// bg3se is by Norbyte and the bg3se contributors; only the search is ours.

#include <stdafx.h>

#include <CoreLib/Base/BaseString.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>
#include <string>

#include "../log.h"
#include "../mem.h"

namespace bg3le {
namespace {

using bg3se::FixedString;
using bg3se::GlobalStringTable;

using SubTable = GlobalStringTable::SubTable;
using Header = bg3se::FixedStringBase::Header;

// The engine's stride between sub-tables.
//
// Not sizeof(SubTable): see the file comment. bg3se's field names record the
// offsets they were found at -- field_1180 at 0x1180, field_11E8 at 0x11E8 --
// and the struct ends with three more qwords, which puts the engine's
// SubTable at 0x1200. Assumed here and then verified against eleven
// sub-tables, so a wrong value finds nothing rather than finding rubbish.
constexpr std::size_t kSubTableStride = 0x1200;

// Every offset the walk needs, and each one is below the CRITICAL_SECTION that
// makes the total size diverge, so these come from the struct itself.
static_assert(offsetof(SubTable, TableIndex) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, EntrySize) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, EntriesPerBucket) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, NumBuckets) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, Buckets) < offsetof(SubTable, CriticalSection));
static_assert(offsetof(SubTable, CriticalSection) < kSubTableStride,
              "the stride has to leave room for everything before it");

// A FixedString is just the index, so a field holding one is four bytes.
static_assert(sizeof(FixedString) == sizeof(std::uint32_t));

constexpr std::size_t kSubTableCount =
    sizeof(GlobalStringTable::SubTables) / sizeof(GlobalStringTable::SubTable);

template <class T>
T read_at(void const* base, std::size_t offset) {
    T value{};
    std::memcpy(&value, (char const*)base + offset, sizeof(T));
    return value;
}

void const* sub_table(void const* table, std::size_t index) {
    return (char const*)table + index * kSubTableStride;
}

// Whether a sub-table's own fields are self-consistent. Deliberately loose:
// this only has to reject the overwhelming majority of coincidences, and the
// string check afterwards is what actually confirms the find.
bool sub_table_plausible(void const* st) {
    const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
    const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
    const auto buckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
    const auto bucketPtr = read_at<void*>(st, offsetof(SubTable, Buckets));

    if (entrySize < sizeof(Header) + 1 || entrySize > 0x10000) return false;
    if (perBucket == 0 || perBucket > (1u << 24)) return false;
    if (buckets == 0 || buckets > (1u << 24)) return false;
    if (bucketPtr == nullptr) return false;
    return true;
}

// The address of a string, or null. The index arithmetic is bg3se's.
char const* resolve(void const* table, std::uint32_t id, std::uint32_t* length) {
    if (table == nullptr || id == FixedString::NullIndex) return nullptr;

    const std::size_t subTableIdx = id & 0x0F;
    if (subTableIdx >= kSubTableCount) return nullptr;

    void const* st = sub_table(table, subTableIdx);
    const std::size_t bucketIdx = (id >> 4) & 0xffff;
    const std::size_t entryIdx = (id >> 20);

    const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
    const auto numBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
    if (bucketIdx >= numBuckets || entryIdx >= perBucket) return nullptr;

    const auto entrySize = read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
    auto** buckets = read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
    if (buckets == nullptr) return nullptr;

    std::uint8_t* bucket = nullptr;
    if (!safe_read(&buckets[bucketIdx], &bucket, sizeof(bucket))
        || bucket == nullptr) {
        return nullptr;
    }

    auto const* header = (Header const*)(bucket + entryIdx * entrySize);
    Header copy{};
    if (!safe_read(header, &copy, sizeof(copy))) return nullptr;
    if (copy.Length > entrySize) return nullptr;

    if (length != nullptr) *length = copy.Length;
    return (char const*)(header + 1);
}

// Confirms a candidate by resolving something out of it and checking the
// string against its own recorded length. A structure that merely looks like
// the table will not survive this.
bool table_resolves_a_string(void const* table) {
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto numBuckets = read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto perBucket = read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        for (std::uint32_t bucket = 0; bucket < numBuckets && bucket < 64; ++bucket) {
            for (std::uint32_t entry = 0; entry < perBucket && entry < 8; ++entry) {
                const std::uint32_t id =
                    (std::uint32_t)sub | (bucket << 4) | (entry << 20);
                std::uint32_t length = 0;
                char const* str = resolve(table, id, &length);
                if (str == nullptr || length == 0 || length > 512) continue;

                char buf[513];
                if (!safe_read(str, buf, length + 1)) continue;
                if (buf[length] != '\0') continue;

                bool printable = true;
                for (std::uint32_t i = 0; i < length; ++i) {
                    if ((unsigned char)buf[i] < 0x20 || (unsigned char)buf[i] > 0x7e) {
                        printable = false;
                        break;
                    }
                }
                if (!printable) continue;

                buf[length] = '\0';
                logf("string table: resolved id %#x to \"%s\"", id, buf);
                return true;
            }
        }
    }
    return false;
}

// Eleven consecutive sub-table indices at the expected stride.
bool looks_like_table(void const* candidate, std::size_t available) {
    const std::size_t span = (kSubTableCount - 1) * kSubTableStride
                             + offsetof(SubTable, TableIndex) + sizeof(std::int32_t);
    if (available < span) return false;

    for (std::size_t k = 0; k < kSubTableCount; ++k) {
        const auto index = read_at<std::int32_t>(
            sub_table(candidate, k), offsetof(SubTable, TableIndex));
        if (index != (std::int32_t)k) return false;
    }
    return true;
}

void* g_table = nullptr;
bool g_searched = false;

}  // namespace

// Whether a /proc/self/maps line describes a region worth scanning, and its
// bounds.
//
// Exported so it can be checked against real map lines without a game. The
// first version of this skipped two fields before looking for the pathname,
// but the format is
//
//   address perms offset dev inode pathname
//
// so it landed on the inode, saw a digit, concluded every region was
// file-backed, and scanned nothing at all -- reporting "not found" for a table
// it had never looked for. The logging caught that; a test would have caught
// it sooner.
//
// Anonymous writable mappings only, which is not a guess about where the table
// lives: bg3se reaches it through a GlobalStringTable**, so the variable holds
// a pointer and the table itself is a heap allocation. Two other reasons agree.
// The scan reads directly rather than through safe_read, because safe_read is
// a process_vm_readv syscall and this makes hundreds of millions of probes --
// and a direct read of a file-backed page can raise SIGBUS if the file has
// been truncated, where an anonymous page cannot. So the fast path is also the
// safe one.
extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to) {
    char perms[8] = {0};
    int consumed = 0;
    if (std::sscanf(line, "%llx-%llx %7s %*s %*s %*s %n", from, to, perms,
                    &consumed) < 3) {
        return false;
    }
    if (perms[0] != 'r' || perms[1] != 'w') return false;
    if (*to <= *from) return false;

    char const* path = line + consumed;
    while (*path == ' ') ++path;

    // No pathname at all, or one of the kernel's bracketed names. [heap] and
    // [stack] are anonymous and fine to read; the two below are not ordinary
    // memory.
    if (*path == '\0' || *path == '\n') return true;
    if (*path != '[') return false;
    if (std::strncmp(path, "[vvar", 5) == 0) return false;
    if (std::strncmp(path, "[vsyscall", 9) == 0) return false;
    return true;
}

namespace {

bool scannable_region(char const* line, unsigned long long* from,
                      unsigned long long* to) {
    return bg3le_scannable_region(line, from, to);
}

// Scans every writable region, in address order.
void* search_for_table() {
    // Logged rather than left as a claim in the comment above: this is the
    // divergence that makes sizeof(SubTable) unusable as the stride.
    logf("string table: searching with stride %#zx; this build computes "
         "sizeof(SubTable) as %#zx, %zu bytes larger, because the Linux "
         "critical-section stand-in is wider than the Windows type",
         kSubTableStride, sizeof(SubTable),
         sizeof(SubTable) - kSubTableStride);

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;

    char line[512];
    std::size_t regions = 0;
    std::size_t scanned = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!scannable_region(line, &from, &to)) continue;

        ++regions;
        const std::size_t size = (std::size_t)(to - from);
        if (size < kSubTableCount * kSubTableStride) continue;
        scanned += size;

        for (unsigned long long addr = from; addr + kSubTableStride <= to;
             addr += 8) {
            void const* candidate = (void const*)addr;
            if (!looks_like_table(candidate, (std::size_t)(to - addr))) continue;
            if (!sub_table_plausible(candidate)) continue;
            if (!table_resolves_a_string(candidate)) continue;

            std::fclose(maps);
            logf("string table: found at %p (scanned %zu bytes over %zu "
                 "regions)", candidate, scanned, regions);
            return (void*)candidate;
        }
    }

    std::fclose(maps);
    logf("string table: not found (scanned %zu bytes over %zu regions); "
         "FixedString fields stay unreadable", scanned, regions);
    return nullptr;
}

}  // namespace

// Finds the table on first use and caches the answer.
//
// Deliberately lazy: the engine builds the table during startup, so a search
// at load time would find nothing and cache that.
extern "C" void* bg3le_string_table() {
    if (!g_searched) {
        g_searched = true;
        g_table = search_for_table();
    }
    return g_table;
}

// The index of a string, by its text, or false if the table does not hold it.
//
// This exists so a scan can look for one exact 32-bit value instead of
// guessing at plausible indices. The stats search first tried the latter:
// seven plausibility tests at every four-byte offset of every writable
// region, which took 114 seconds. Searching for a known index is a plain
// memory compare.
//
// Walks buckets rather than entries. Resolving an entry costs a
// process_vm_readv each, and a table of this size has millions of them, so
// each bucket is read whole and searched locally -- one syscall per bucket.
extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out) {
    if (wanted == nullptr) return false;
    void const* table = bg3le_string_table();
    if (table == nullptr) return false;

    const std::size_t wantLen = std::strlen(wanted);
    std::vector<unsigned char> bucketBuf;

    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
        if (buckets == nullptr || entrySize == 0) continue;

        const std::size_t span = (std::size_t)perBucket * (std::size_t)entrySize;
        if (span == 0 || span > (64u << 20)) continue;
        bucketBuf.resize(span);

        for (std::uint32_t b = 0; b < numBuckets; ++b) {
            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[b], &bucket, sizeof(bucket))
                || bucket == nullptr) {
                continue;
            }
            const std::size_t got =
                safe_read_some(bucket, bucketBuf.data(), span);
            if (got < sizeof(Header) + 1) continue;

            for (std::uint32_t e = 0; e < perBucket; ++e) {
                const std::size_t at = (std::size_t)e * (std::size_t)entrySize;
                if (at + sizeof(Header) + wantLen + 1 > got) break;

                auto const* header = (Header const*)(bucketBuf.data() + at);
                if (header->Length != wantLen) continue;

                char const* text = (char const*)(header + 1);
                if (std::memcmp(text, wanted, wantLen) != 0) continue;
                if (text[wantLen] != '\0') continue;

                if (out != nullptr) {
                    *out = (std::uint32_t)sub | (b << 4) | (e << 20);
                }
                return true;
            }
        }
    }
    return false;
}

// The text of a FixedString index, or null. length may be null.
extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length) {
    void* table = bg3le_string_table();
    if (table == nullptr) return nullptr;
    return resolve(table, index, length);
}


// A FixedString for text the game does not already hold.
//
// Needed because a string attribute on a stat holds an index into a pool of
// FixedString ids, so a mod assigning one -- 5eSpells appends to
// PotentSpellcasting.Boosts -- needs an id for text that has never existed.
//
// The sub-tables are length classes: entry sizes 48 through 2080, each
// holding the header and up to entrySize - 0x18 bytes of text. Entries
// inside a class are allocated out of fixed-size buckets, and field_1100
// counts how many have been taken -- 42,275 of sub 0's 32 x 1365, 5,325 of
// sub 7's 29 x 186 -- so the next entry the engine itself would hand out is
// the one at that index, and taking it and incrementing the count is what
// the engine does. That is the whole point of doing it this way rather than
// writing into unused space: space the engine still considers free is space
// it will hand to someone else.
//
// Nothing is added to the hash map the engine interns through, which is a
// deliberate limitation rather than an oversight: the map's layout is not
// established here, and the only consequence of staying out of it is that
// the engine interning the same text later makes its own second entry. Two
// entries with the same text is what the table looks like anyway when a
// string arrives twice before either is released.
//
// The refcount is set high enough never to reach zero. An entry released
// down to zero would go back on the engine's free list, and the id a mod is
// holding would then be handed to someone else's text.
//
// Verified before anything is written: the entry about to be taken, and the
// ones after it, have to read as unused. That is what tells the tail of the
// allocated region from a hole inside it -- field_1100 is a high-water
// mark, not a count of live entries, so the entry below it is often free
// too. Requiring that neighbour to be *used* was the first attempt and it
// refused a write it should have allowed, which is the right direction for
// a check to fail in.
constexpr std::uint32_t kInternRefCount = 0x100000;

extern "C" bool bg3le_fixed_string_index_of(char const* wanted,
                                            std::uint32_t* out);

extern "C" bool bg3le_fixed_string_intern(char const* text,
                                          std::uint32_t* out) {
    if (text == nullptr || out == nullptr) return false;

    // What has already been interned here, so the same text asked for
    // twice costs nothing.
    //
    // Deliberately not a search of the engine's table first. That search
    // reads every bucket of every sub-table -- about a million and a half
    // entries -- and at eighty milliseconds a call it turned a mod's stat
    // pass into minutes. The cost of skipping it is a second entry for
    // text the table already holds, which is what the table looks like
    // anyway whenever a string arrives twice before either copy is
    // released.
    static std::unordered_map<std::string, std::uint32_t> ours;
    auto known = ours.find(text);
    if (known != ours.end()) {
        *out = known->second;
        return true;
    }

    void const* table = bg3le_string_table();
    if (table == nullptr) return false;

    const std::size_t length = std::strlen(text);
    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        if (entrySize <= sizeof(Header) + 1) continue;
        if (length + 1 > entrySize - sizeof(Header)) continue;  // next class

        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));
        const auto used = read_at<std::uint32_t>(st, 0x1100);
        if (perBucket == 0 || buckets == nullptr) continue;

        const std::uint64_t capacity =
            (std::uint64_t)perBucket * (std::uint64_t)numBuckets;
        if (used == 0 || used >= capacity) {
            logf("string table: sub-table %zu holds %u of %llu entries, so "
                 "there is no room for %zu bytes of text; the engine grows "
                 "this by allocating a bucket, which bg3le does not do",
                 sub, used, (unsigned long long)capacity, length);
            return false;
        }

        auto entry_at = [&](std::uint32_t index) -> std::uint8_t* {
            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[index / perBucket], &bucket,
                           sizeof(bucket))
                || bucket == nullptr) {
                return nullptr;
            }
            return bucket + (std::size_t)(index % perBucket) * entrySize;
        };

        std::uint8_t* target = entry_at(used);
        if (target == nullptr) return false;

        // The entry being taken and its followers, all of which have to be
        // untouched: a run of free entries is the tail, a free entry with
        // used ones after it is a hole and belongs to the engine's free
        // list.
        constexpr std::uint32_t kRun = 4;
        for (std::uint32_t ahead = 0; ahead < kRun; ++ahead) {
            const std::uint32_t index = used + ahead;
            if (index >= capacity) break;
            if (index / perBucket != used / perBucket) break;  // next bucket

            std::uint8_t* at = entry_at(index);
            if (at == nullptr) break;

            Header h{};
            if (!safe_read(at, &h, sizeof(h))) return false;
            if (h.RefCount == 0 && h.Length == 0) continue;

            logf("string table: entry %u of sub-table %zu reads refs %u len "
                 "%u, so entry %u is a hole in the allocated region rather "
                 "than the tail of it; nothing written", index, sub,
                 h.RefCount, h.Length, used);
            return false;
        }

        Header header{};
        header.Hash = 0;  // the hash map is not touched; see above
        header.RefCount = kInternRefCount;
        header.Length = (std::uint32_t)length;
        // Every live entry in this table reads 1 here, whatever the field
        // means; it is plainly not the entry's own id.
        header.Id = 1;
        header.NextFreeIndex = 0;

        std::memcpy(target, &header, sizeof(header));
        std::memcpy(target + sizeof(Header), text, length + 1);

        // BG3LE_STRING_TABLE_BUMP=0 leaves the engine's own entry counter
        // alone, so "is incrementing it what upsets the engine?" can be
        // answered without a rebuild. Leaving it alone means the engine
        // will eventually hand the same entry to someone else.
        char const* bump = std::getenv("BG3LE_STRING_TABLE_BUMP");
        if (bump == nullptr || bump[0] != '0') {
            const auto next = (std::uint32_t)(used + 1);
            std::memcpy((void*)((char*)st + 0x1100), &next, sizeof(next));
        }

        const std::uint32_t id = (std::uint32_t)sub
                                 | ((used / perBucket) << 4)
                                 | ((used % perBucket) << 20);

        // Proved by reading it back the way everything else reads one.
        std::uint32_t got = 0;
        char const* back = resolve(table, id, &got);
        if (back == nullptr || got != length
            || std::strcmp(back, text) != 0) {
            logf("string table: wrote \"%s\" as id %#x but it reads back as "
                 "%s; the entry is left in place and refcounted so nothing "
                 "reuses it", text, id, back == nullptr ? "nothing" : back);
            return false;
        }

        // Quiet after the first few: a mod's stat pass interns dozens.
        static std::size_t said = 0;
        if (++said <= 3) {
            logf("string table: interned %zu bytes as id %#x in sub-table "
                 "%zu (entry %u of %llu): \"%.64s%s\"", length, id, sub,
                 used, (unsigned long long)capacity, text,
                 length > 64 ? "..." : "");
        }

        ours.emplace(text, id);
        *out = id;
        return true;
    }

    logf("string table: no sub-table takes %zu bytes of text", length);
    return false;
}

// What the sub-tables look like, and whether an entry can be taken from
// one the way the engine takes one. BG3LE_DUMP_STRINGTABLE=1.
//
// The question this answers is how to get a FixedString for text the game
// does not already hold, which is what a mod needs when it assigns a
// string attribute it built at runtime. The header carries a
// NextFreeIndex, so each sub-table keeps a free list, and taking an entry
// off it is what the engine itself does -- unlike writing into unused
// space, which the engine would later hand to someone else.
//
// Header.Id is the self-check: an entry records the id it is reachable by,
// so a computed id can be proved against the entry it lands on before
// anything is written.
extern "C" void bg3le_fixed_string_dump() {
    void const* table = bg3le_string_table();
    if (table == nullptr) {
        logf("string table: not located, nothing to dump");
        return;
    }

    for (std::size_t sub = 0; sub < kSubTableCount; ++sub) {
        void const* st = sub_table(table, sub);
        if (!sub_table_plausible(st)) continue;

        const auto entrySize =
            read_at<std::uint64_t>(st, offsetof(SubTable, EntrySize));
        const auto perBucket =
            read_at<std::uint32_t>(st, offsetof(SubTable, EntriesPerBucket));
        const auto numBuckets =
            read_at<std::uint32_t>(st, offsetof(SubTable, NumBuckets));
        const auto f1100 = read_at<std::uint32_t>(st, 0x1100);
        const auto f1180 = read_at<std::uint64_t>(st, 0x1180);
        auto** buckets =
            read_at<std::uint8_t**>(st, offsetof(SubTable, Buckets));

        std::size_t live = 0;
        for (std::uint32_t b = 0; b < numBuckets; ++b) {
            std::uint8_t* bucket = nullptr;
            if (safe_read(&buckets[b], &bucket, sizeof(bucket))
                && bucket != nullptr) {
                ++live;
            }
        }

        logf("string table: sub %zu entrySize %llu perBucket %u buckets %u "
             "(%zu allocated) field_1100 %u field_1180 %llu", sub,
             (unsigned long long)entrySize, perBucket, numBuckets, live,
             f1100, (unsigned long long)f1180);

        // One live entry, to confirm Header.Id is the id it is found by.
        for (std::uint32_t b = 0; b < numBuckets && b < 4; ++b) {
            std::uint8_t* bucket = nullptr;
            if (!safe_read(&buckets[b], &bucket, sizeof(bucket))
                || bucket == nullptr) {
                continue;
            }
            for (std::uint32_t e = 0; e < perBucket && e < 4; ++e) {
                Header h{};
                if (!safe_read(bucket + (std::size_t)e * entrySize, &h,
                               sizeof(h))) {
                    continue;
                }
                const std::uint32_t id =
                    (std::uint32_t)sub | (b << 4) | (e << 20);
                char text[64] = {};
                safe_read(bucket + (std::size_t)e * entrySize + sizeof(Header),
                          text, sizeof(text) - 1);
                logf("string table:   id %#x -> hash %#x refs %u len %u "
                     "Header.Id %#x nextFree %llu \"%.32s\"", id, h.Hash,
                     h.RefCount, h.Length, h.Id,
                     (unsigned long long)h.NextFreeIndex, text);
            }
            break;
        }
    }
}

}  // namespace bg3le
