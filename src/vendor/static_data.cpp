// Finds the engine's GUID resource manager, which is what Ext.StaticData
// reads.
//
// A resource is looked up by type and GUID: the manager maps a static data
// type index to a bank, and the bank maps a GUID to the resource. Half of that
// bg3le already has -- the type indices are named in the symbol table, under
// ls::ImmutableDataHeadmaster, 121 of them. The manager itself is an anonymous
// global with no symbol, like the string table.
//
// Unlike the string table, though, the fingerprint here does not have to be
// invented. The manager is one HashMap<StaticDataTypeIndex,
// GuidResourceBankBase*>, and the keys are indices bg3le already knows: a
// table whose keys are all drawn from those 121 values, with pointers whose
// first word is a vtable, is that manager rather than a coincidence. So the
// search is checked against something independently known rather than against
// a shape that merely looks right.
//
// The layouts are bg3se's, from GameDefinitions/GuidResources.h; only the
// search is ours.

#include <stdafx.h>

#include <GameDefinitions/GuidResources.h>

#include <cstdio>
#include <cstring>

#include "../ecs_types.h"
#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {
namespace {

using bg3se::resource::GuidResourceBankBase;
using bg3se::resource::GuidResourceManager;

// HashMap's layout, which the string table search already relies on:
// HashKeys (StaticArray, 16), NextIds (Array, 16), Keys (Array, 16), then
// Values (UninitializedStaticArray, 16).
constexpr std::size_t kKeysOffset = 32;
constexpr std::size_t kValuesOffset = 48;

// Within an Array: buffer, capacity, size.
constexpr std::size_t kArrayBuffer = 0;
constexpr std::size_t kArraySize = 12;

static_assert(sizeof(GuidResourceManager) == 64,
              "the manager is expected to be one HashMap");

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

// The executable's mapped range, so a vtable pointer can be told from a heap
// pointer. Taken from the main mapping rather than assumed.
struct ImageRange {
    unsigned long long From{0};
    unsigned long long To{0};
};

ImageRange image_range() {
    static ImageRange range = [] {
        ImageRange r;
        std::FILE* maps = std::fopen("/proc/self/maps", "r");
        if (maps == nullptr) return r;

        char line[512];
        while (std::fgets(line, sizeof(line), maps) != nullptr) {
            // The executable's own mappings: the first r-xp region backed by a
            // path ending in /bg3.
            if (std::strstr(line, "/bin/bg3") == nullptr) continue;
            unsigned long long from = 0;
            unsigned long long to = 0;
            if (std::sscanf(line, "%llx-%llx", &from, &to) != 2) continue;
            if (r.From == 0 || from < r.From) r.From = from;
            if (to > r.To) r.To = to;
        }
        std::fclose(maps);
        return r;
    }();
    return range;
}

bool looks_like_vtable(unsigned long long p) {
    const auto range = image_range();
    if (range.To == 0) return p > 0x1000;  // cannot tell; accept anything sane
    return p >= range.From && p < range.To;
}

// Whether a candidate address holds the resource manager's Definitions map.
//
// Every check here is against something known independently: the key set comes
// from the symbol table, and the values have to be objects whose first word
// points into the executable. A HashMap of the right shape holding arbitrary
// integers will not pass.
bool looks_like_manager(void const* candidate) {
    void* keyBuf = nullptr;
    std::uint32_t keyCount = 0;
    void* valueBuf = nullptr;

    auto const* base = (char const*)candidate;
    if (!read_as(base + kKeysOffset + kArrayBuffer, &keyBuf)) return false;
    if (!read_as(base + kKeysOffset + kArraySize, &keyCount)) return false;
    if (!read_as(base + kValuesOffset + kArrayBuffer, &valueBuf)) return false;

    if (keyBuf == nullptr || valueBuf == nullptr) return false;
    // The engine registers on the order of a hundred resource types; a handful
    // or thousands means this is something else.
    if (keyCount < 8 || keyCount > 400) return false;

    for (std::uint32_t i = 0; i < keyCount; ++i) {
        std::int32_t key = 0;
        if (!read_as((char const*)keyBuf + i * sizeof(std::int32_t), &key)) {
            return false;
        }
        if (!ecs::has_index(ecs::Context::ImmutableData, key)) return false;

        unsigned long long bank = 0;
        if (!read_as((char const*)valueBuf + i * sizeof(void*), &bank)) {
            return false;
        }
        if (bank == 0) return false;

        unsigned long long vtable = 0;
        if (!read_as((void const*)bank, &vtable)) return false;
        if (!looks_like_vtable(vtable)) return false;
    }
    return true;
}

void* g_manager = nullptr;
bool g_searched = false;

void* search_for_manager() {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return nullptr;

    char line[512];
    std::size_t regions = 0;
    std::size_t scanned = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        ++regions;
        if ((std::size_t)(to - from) < sizeof(GuidResourceManager)) continue;
        scanned += (std::size_t)(to - from);

        for (unsigned long long addr = from;
             addr + sizeof(GuidResourceManager) <= to; addr += 8) {
            if (!looks_like_manager((void const*)addr)) continue;

            std::fclose(maps);
            logf("static data: resource manager at %#llx (scanned %zu bytes "
                 "over %zu regions)", addr, scanned, regions);
            return (void*)addr;
        }
    }

    std::fclose(maps);
    logf("static data: resource manager not found (scanned %zu bytes over %zu "
         "regions); Ext.StaticData stays unavailable", scanned, regions);
    return nullptr;
}

}  // namespace

// Found on first use and cached, for the same reason as the string table: the
// engine builds it during startup.
extern "C" void* bg3le_resource_manager() {
    if (!g_searched) {
        g_searched = true;
        g_manager = search_for_manager();
    }
    return g_manager;
}

// The bank for a static data type index, or null.
extern "C" void* bg3le_resource_bank(std::int32_t typeIndex) {
    void* manager = bg3le_resource_manager();
    if (manager == nullptr || typeIndex < 0) return nullptr;

    auto* map = reinterpret_cast<GuidResourceManager*>(manager);
    auto* bank = map->Definitions.try_get(
        (bg3se::resource::StaticDataTypeIndex)typeIndex);
    return bank != nullptr ? *bank : nullptr;
}

// How many banks the manager holds, and the index of the i'th, so the set can
// be listed and checked against the registry from script.
extern "C" std::size_t bg3le_resource_bank_count() {
    void* manager = bg3le_resource_manager();
    if (manager == nullptr) return 0;
    return reinterpret_cast<GuidResourceManager*>(manager)
        ->Definitions.keys().size();
}

extern "C" bool bg3le_resource_bank_at(std::size_t i, std::int32_t* typeIndex,
                                       void** bank) {
    void* manager = bg3le_resource_manager();
    if (manager == nullptr) return false;

    auto* map = reinterpret_cast<GuidResourceManager*>(manager);
    auto const& keys = map->Definitions.keys();
    if (i >= keys.size()) return false;

    *typeIndex = (std::int32_t)keys[(std::uint32_t)i];
    *bank = map->Definitions.values()[i];
    return true;
}

}  // namespace bg3le
