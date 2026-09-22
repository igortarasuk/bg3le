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
#include <cstring>
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

// Scans the writable anonymous mappings. The table is a heap allocation, so
// file-backed regions are skipped, which is most of the address space.
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
        char perms[8] = {0};
        int consumed = 0;
        if (std::sscanf(line, "%llx-%llx %7s %*s %*s %n", &from, &to, perms,
                        &consumed) < 3) {
            continue;
        }
        if (perms[0] != 'r' || perms[1] != 'w') continue;

        // Anonymous only: a path after the fields means file-backed.
        const char* rest = line + consumed;
        while (*rest == ' ') ++rest;
        const bool anonymous = (*rest == '\n' || *rest == '\0' || *rest == '[');
        if (!anonymous) continue;

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
                 "anonymous regions)", candidate, scanned, regions);
            return (void*)candidate;
        }
    }

    std::fclose(maps);
    logf("string table: not found (scanned %zu bytes over %zu anonymous "
         "regions); FixedString fields stay unreadable",
         scanned, regions);
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

// The text of a FixedString index, or null. length may be null.
extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length) {
    void* table = bg3le_string_table();
    if (table == nullptr) return nullptr;
    return resolve(table, index, length);
}

}  // namespace bg3le
