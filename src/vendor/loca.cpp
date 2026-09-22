// The translated string repository, which is what Ext.Loca reads and what
// turns the loca handles Ext.Stats reports into text.
//
// ls::TranslatedStringRepository has no symbol -- only the type-id statics
// survive, as everywhere else in the engine -- and it hangs off nothing
// bg3le already holds. So it is found by content, and the fingerprint is
// unusually good: the repository's text pools are
// HashMap<RuntimeStringHandle, LSStringView>, and a loca handle is a
// FixedString whose text is an "h" followed by thirty-six characters.
// Several hundred consecutive keys all matching that shape is not a
// coincidence any other map in the process produces.
//
// What is built here is bg3le's own index rather than a view onto the
// engine's: the keys and values are parallel arrays, so one pass copies
// them out, and afterwards a lookup costs nothing and cannot be caught
// mid-rehash by the game.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

namespace {

// A bg3se HashMap: HashKeys, NextIds, Keys, Values.
constexpr std::size_t kKeysBuffer = 32;
constexpr std::size_t kKeysCapacity = 40;
constexpr std::size_t kKeysSize = 44;
constexpr std::size_t kValuesBuffer = 48;

// RuntimeStringHandle is a FixedString and a version, padded to eight.
constexpr std::size_t kKeyStride = 8;
// LSStringView is a pointer and a length.
constexpr std::size_t kValueStride = 16;

// A loca handle: 'h' then thirty-six hex-and-g characters.
constexpr std::size_t kHandleLength = 37;

constexpr std::uint32_t kMinEntries = 500;
constexpr std::uint32_t kMaxEntries = 1u << 20;

struct Repository {
    bool Built{false};
    std::unordered_map<std::string, std::string> ByHandle;
};

Repository& state() {
    static Repository r;
    return r;
}

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

bool is_handle(char const* text) {
    return text != nullptr && text[0] == 'h'
           && std::strlen(text) == kHandleLength;
}

// Whether a candidate map's keys read as loca handles.
bool keys_look_like_handles(void const* keys, std::uint32_t count) {
    const std::uint32_t probe = count < 16 ? count : 16;
    std::uint32_t good = 0;
    for (std::uint32_t i = 0; i < probe; ++i) {
        std::uint32_t index = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &index)) return false;
        if (is_handle(bg3le_fixed_string(index, nullptr))) ++good;
    }
    // Every one of them: a pool holds nothing else.
    return probe > 0 && good == probe;
}

// One LSStringView, if it points at text.
bool read_view(void const* at, std::string* out) {
    std::uint64_t data = 0;
    std::uint64_t size = 0;
    if (!read_as(at, &data) || !read_as((char const*)at + 8, &size)) {
        return false;
    }
    if (data == 0) {
        out->clear();
        return true;
    }
    if (size > (1u << 20)) return false;

    out->assign(size, '\0');
    if (size == 0) return true;
    return safe_read((void const*)(std::uintptr_t)data, out->data(), size);
}

bool harvest(void const* map, Repository* into) {
    void const* keys = nullptr;
    void const* values = nullptr;
    std::uint32_t capacity = 0;
    std::uint32_t count = 0;
    if (!read_as((char const*)map + kKeysBuffer, &keys)
        || !read_as((char const*)map + kKeysCapacity, &capacity)
        || !read_as((char const*)map + kKeysSize, &count)
        || !read_as((char const*)map + kValuesBuffer, &values)) {
        return false;
    }
    if (count < kMinEntries || count > kMaxEntries || count > capacity) {
        return false;
    }
    if (keys == nullptr || values == nullptr) return false;
    if (!keys_look_like_handles(keys, count)) return false;

    std::size_t added = 0;
    std::string text;
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t index = 0;
        if (!read_as((char const*)keys + i * kKeyStride, &index)) break;

        char const* handle = bg3le_fixed_string(index, nullptr);
        if (!is_handle(handle)) continue;
        if (!read_view((char const*)values + i * kValueStride, &text)) {
            continue;
        }

        // Only entries that actually carry text. A map whose keys are
        // handles but whose values read empty is not a text pool, and
        // harvesting it would answer every lookup with "" -- which a
        // caller cannot tell from a string that is genuinely empty.
        if (text.empty()) continue;

        // The first pool to define a handle wins, which is the order the
        // engine consults them in.
        if (into->ByHandle.emplace(handle, text).second) ++added;
    }
    return added > 0;
}

extern "C" bool bg3le_fixed_string_index_of(char const* text,
                                            std::uint32_t* out);

// Under BG3LE_DUMP_LOCA: find a handle we know exists by its FixedString
// index and show what surrounds each occurrence, so the pool's real shape
// can be read rather than assumed. The fingerprint search below found
// nothing, which means one of its assumptions about the key encoding is
// wrong.
void dump_handle_sites(char const* handle) {
    std::uint32_t needle = 0;
    if (!bg3le_fixed_string_index_of(handle, &needle)) {
        logf("locadump: %s is not an interned string", handle);
        return;
    }
    logf("locadump: %s is FixedString %u", handle, needle);

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return;

    constexpr std::size_t kChunk = 1u << 20;
    static std::vector<unsigned char> block;
    block.resize(kChunk + 8);

    char line[512];
    std::size_t hits = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr && hits < 6) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to && hits < 6;
             base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + 8) want = kChunk + 8;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < 8) continue;

            for (std::size_t off = 0; off + 4 <= got && hits < 6; off += 4) {
                std::uint32_t value = 0;
                std::memcpy(&value, block.data() + off, sizeof(value));
                if (value != needle) continue;
                ++hits;

                logf("locadump: hit %zu at %#llx", hits,
                     (unsigned long long)(base + off));
                for (int rel = -16; rel <= 32; rel += 8) {
                    std::uint64_t word = 0;
                    if (!read_as((char const*)(std::uintptr_t)
                                     ((long long)(base + off) + rel),
                                 &word)) {
                        continue;
                    }
                    char text[41] = {};
                    char note[64] = {};
                    if (word > 0x1000 && word < 0x7fffffffffffull
                        && safe_read_some((void const*)(std::uintptr_t)word,
                                          text, 40) > 0
                        && text[0] >= 0x20 && text[0] < 0x7f) {
                        std::snprintf(note, sizeof(note), " -> \"%.32s\"",
                                      text);
                    }
                    logf("locadump:   %+3d %016llx%s", rel,
                         (unsigned long long)word, note);
                }
            }
        }
    }
    std::fclose(maps);
}

bool search() {
    Repository found{};

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kHeader = 64;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kHeader);

    char line[512];
    std::size_t pools = 0;
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kHeader) want = kChunk + kHeader;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < kHeader) continue;

            for (std::size_t off = 0; off + kHeader <= got; off += 8) {
                // A cheap rejection before the reads: the key count has to
                // be in range and no bigger than the capacity.
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

                if (harvest((void const*)(base + off), &found)) ++pools;
            }
        }
    }
    std::fclose(maps);

    // Fail closed. The fingerprint -- a hash map whose keys are loca
    // handles -- matches structures that are not text pools: two runs
    // harvested 956 and 3 entries respectively, every one of them with no
    // text. Publishing that would make Ext.Loca answer "" for everything,
    // which is indistinguishable from a real empty string. Until the pool
    // is identified properly, a handful of entries means it was not found.
    constexpr std::size_t kMinStrings = 1000;
    if (found.ByHandle.size() < kMinStrings) {
        logf("loca: found %zu strings across %zu candidate pools, fewer "
             "than the %zu a real localisation holds; treating it as not "
             "found", found.ByHandle.size(), pools, kMinStrings);
        found.ByHandle.clear();
    }

    if (found.ByHandle.empty()) {
        logf("loca: no translated string pool found; Ext.Loca stays "
             "unavailable");

        return false;
    }

    found.Built = true;
    state() = std::move(found);
    logf("loca: %zu translated strings from %zu pools",
         state().ByHandle.size(), pools);
    if (std::getenv("BG3LE_DUMP_LOCA") != nullptr) {
        dump_handle_sites("h978e1507gf90eg49b5ga5f8g5b1c0548d77f");
    }
    return true;
}

bool ready() {
    if (state().Built) return true;

    // As many attempts as the warm thread makes. Eight ran out forty
    // seconds in, which is before the save has loaded and therefore
    // before the repository is populated -- it never got a chance.
    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return search();
}

}  // namespace

extern "C" bool bg3le_loca_ready() { return ready(); }

extern "C" char const* bg3le_loca_get(char const* handle) {
    if (handle == nullptr || !ready()) return nullptr;

    auto it = state().ByHandle.find(handle);
    if (it == state().ByHandle.end()) return nullptr;
    return it->second.c_str();
}

extern "C" std::size_t bg3le_loca_count() {
    return ready() ? state().ByHandle.size() : 0;
}

// The handles, for GetAllTranslatedStringKeys. Stable across calls because
// the index is bg3le's own copy and never rebuilt once found.
extern "C" char const* bg3le_loca_handle_at(std::size_t index) {
    if (!ready()) return nullptr;

    static std::vector<char const*> order;
    if (order.size() != state().ByHandle.size()) {
        order.clear();
        order.reserve(state().ByHandle.size());
        for (auto const& entry : state().ByHandle) {
            order.push_back(entry.first.c_str());
        }
    }
    return index < order.size() ? order[index] : nullptr;
}

}  // namespace bg3le
