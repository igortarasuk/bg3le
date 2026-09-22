// Finds the engine's module list, which is what Ext.Mod reads.
//
// The mod manager hangs off esv::EoCServer upstream, reached through a
// symbol bg3se recovers by pattern-scanning a Windows image. The Linux build
// names neither: no EoCServer, no ModManager, nothing for LoadOrderedModules.
//
// It does not need one. Every install has the base module "Shared", its UUID
// is the constant ed539163-bb70-431b-96a7-f5b2eda5376b, and it is first in
// load order -- so sixteen known bytes locate Module[0] directly. That is an
// exact-value scan rather than a structural guess, which is the distinction
// that decided every other search in this project: a structural test
// ("an array of named things") matches the wrong array, a known value does
// not.
//
// ModuleInfo's layout is the Windows struct with one substitution. Larian's
// string is sixteen bytes here, not std::string's thirty-two, which is why
// sizeof(Module) upstream (264+) does not match the stride found in memory
// (240). Every field was then checked against a module whose values are
// known from reference/mod-shape.txt: "Shared" inline at +32 with its length
// in the sixteenth byte, four empty FixedStrings where the level names are,
// PhotoBoothLevelName resolving to SYS_PortraitGeneration_A, and the packed
// version at +72 reading back as exactly 1.0.233.3395918.

#include <stdafx.h>

#include <GameDefinitions/Module.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "ls_string.h"
#include "mods.h"

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);
extern "C" bool bg3le_meta_parse_guid(char const* text, void* out);

namespace {

// The base module, present in every install.
constexpr char const* kSharedUuid = "ed539163-bb70-431b-96a7-f5b2eda5376b";

// Within Module: the vtable, then ModuleInfo, whose UUID follows a
// FixedString and its padding.
constexpr std::size_t kUuidInModule = 16;

// Within ModuleInfo: the UUID string index precedes the UUID itself.
constexpr std::size_t kUuidStringBeforeUuid = 8;

// ModuleInfo, relative to the Module. Established from known values; see the
// note at the top of the file.
constexpr std::size_t kInfoName = 32;
constexpr std::size_t kInfoStartLevel = 48;
constexpr std::size_t kInfoMenuLevel = 52;
constexpr std::size_t kInfoLobbyLevel = 56;
constexpr std::size_t kInfoCharCreationLevel = 60;
constexpr std::size_t kInfoPhotoBoothLevel = 64;
constexpr std::size_t kInfoModVersion = 72;
constexpr std::size_t kInfoPublishVersion = 80;
constexpr std::size_t kInfoHash = 88;
constexpr std::size_t kInfoDirectory = 104;
constexpr std::size_t kInfoAuthor = 136;
constexpr std::size_t kInfoDescription = 152;
constexpr std::size_t kInfoNumPlayers = 168;
constexpr std::size_t kInfoFileSize = 176;
constexpr std::size_t kInfoPublishHandle = 184;

// Module's three ModuleShortDesc arrays, which follow ModuleInfo.
constexpr std::size_t kModuleLists[3] = {192, 208, 224};

// ModuleShortDesc, same substitution applied to the upstream struct.
constexpr std::size_t kDescUuidString = 0;
constexpr std::size_t kDescName = 24;
constexpr std::size_t kDescModVersion = 40;
constexpr std::size_t kDescPublishVersion = 48;
constexpr std::size_t kDescHash = 56;
constexpr std::size_t kDescFolder = 72;
constexpr std::size_t kDescPublishHandle = 88;
constexpr std::size_t kDescStride = 96;

// An Array<T> header: buffer, capacity, size.
constexpr std::size_t kArrayHeader = 16;

// Around LoadOrderedModules inside ModManager. BaseModule is the member just
// ahead of it -- one module plus the two flag bytes' padding -- and
// AvailableMods the array just after. Both were confirmed against the
// running game: header-248 names the campaign module, and header+16 is a
// sixteen-entry module array where the load order holds thirteen.
constexpr std::size_t kBaseModuleBeforeLoadOrder = 248;
constexpr std::size_t kAvailableAfterLoadOrder = kArrayHeader;

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

struct Modules {
    void const* Buffer{nullptr};
    std::size_t Count{0};
    std::size_t Stride{0};
};

struct Manager {
    Modules LoadOrder;
    Modules Available;
    void const* BaseModule{nullptr};
};

Manager& state() {
    static Manager m;
    return m;
}

// A module's UUID as text, from its own UUIDString index rather than by
// formatting the bytes, so a mismatch between the two shows up as a failure
// instead of being papered over.
char const* uuid_string_at(void const* module) {
    std::uint32_t index = 0;
    auto const* at = (char const*)module + kUuidInModule
                     - kUuidStringBeforeUuid;
    if (!read_as(at, &index)) return nullptr;
    return bg3le_fixed_string(index, nullptr);
}

// Whether every entry at this stride carries a UUID string that resolves.
// Every entry, not a sample: a 698-entry run of garbage passed a six-entry
// probe because its first sixteen slots were a real module array.
bool array_holds(void const* buffer, std::size_t count, std::size_t stride) {
    for (std::size_t i = 0; i < count; ++i) {
        char const* text = uuid_string_at((char const*)buffer + i * stride);
        if (text == nullptr) return false;
        // A UUID string is 36 characters; anything else is a coincidence.
        if (std::strlen(text) != 36) return false;
    }
    return true;
}

// The stride that makes every entry a module. sizeof(Module) as this build
// computes it is not the engine's, as the stats work showed repeatedly, so it
// is derived rather than assumed.
std::size_t derive_stride(void const* buffer, std::size_t count) {
    for (std::size_t stride = 64; stride <= 2048; stride += 8) {
        if (array_holds(buffer, count, stride)) return stride;
    }
    return 0;
}

// Walks every readable region once, handing each chunk to `visit`.
template <class Visit>
void scan_memory(Visit visit) {
    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kOverlap = 16;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kOverlap);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kOverlap) want = kChunk + kOverlap;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < kOverlap) continue;
            visit(base, block.data(), got);
        }
    }
    std::fclose(maps);
}

struct Header {
    unsigned long long At{0};
    Modules Array;
};

// Under BG3LE_DUMP_MODULE: one module's bytes annotated with anything
// recognisable in them -- resolvable FixedString indices and pointers to
// text. This is how the field offsets above were read off rather than
// predicted.
void dump_module(void const* module, std::size_t stride) {
    std::vector<unsigned char> bytes(stride);
    const std::size_t got = safe_read_some(module, bytes.data(), bytes.size());
    logf("moddump: %zu bytes at %p", got, module);
    for (std::size_t off = 0; off + 8 <= got; off += 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, bytes.data() + off, sizeof(word));
        auto const lo = (std::uint32_t)(word & 0xffffffffu);

        char note[192];
        note[0] = '\0';
        char const* fs = lo != 0 ? bg3le_fixed_string(lo, nullptr) : nullptr;
        std::size_t used = 0;
        if (fs != nullptr) {
            used = (std::size_t)std::snprintf(note, sizeof(note),
                                              " fs=%.48s", fs);
        }
        if (word > 0x1000 && word < 0x7fffffffffffull) {
            char text[41] = {};
            if (safe_read_some((void const*)(std::uintptr_t)word, text, 40)
                > 0) {
                bool printable = text[0] >= 0x20 && text[0] < 0x7f;
                for (std::size_t i = 0; i < 40 && printable; ++i) {
                    if (text[i] == '\0') break;
                    if (text[i] < 0x20 || text[i] >= 0x7f) printable = false;
                }
                if (printable) {
                    std::snprintf(note + used, sizeof(note) - used,
                                  " ->\"%s\"", text);
                }
            }
        }
        logf("moddump: +%3zu %016llx%s", off, (unsigned long long)word, note);
    }
}

// ModManager::BaseModule, the member just ahead of LoadOrderedModules. A
// wrong offset gives an unresolvable uuid, so it is checked before it is
// kept.
void const* base_module_before(unsigned long long header) {
    if (header < kBaseModuleBeforeLoadOrder) return nullptr;
    auto const* at = (char const*)(std::uintptr_t)(
        header - kBaseModuleBeforeLoadOrder);
    char const* uuid = uuid_string_at(at);
    if (uuid == nullptr || std::strlen(uuid) != 36) return nullptr;
    return at;
}

// ModManager::AvailableMods, the array just after LoadOrderedModules.
Modules available_after(unsigned long long header, std::size_t stride) {
    std::uint64_t buffer = 0;
    std::uint32_t capacity = 0;
    std::uint32_t size = 0;
    auto const* at = (char const*)(std::uintptr_t)(
        header + kAvailableAfterLoadOrder);
    if (!read_as(at, &buffer) || !read_as(at + 8, &capacity)
        || !read_as(at + 12, &size)) {
        return Modules{};
    }
    if (size == 0 || size > 4096 || size > capacity) return Modules{};

    auto const* mods = (void const*)(std::uintptr_t)buffer;
    if (!array_holds(mods, size, stride)) return Modules{};
    return Modules{mods, size, stride};
}

bool search() {
    std::uint8_t needle[16] = {};
    if (!bg3le_meta_parse_guid(kSharedUuid, needle)) {
        logf("mods: could not parse the base module's uuid");
        return false;
    }

    // Pass one: every place the base module's guid appears. Each is a
    // candidate Module, since the guid sits at a known offset within one.
    std::vector<std::uint64_t> candidates;
    scan_memory([&](unsigned long long base, unsigned char const* block,
                    std::size_t got) {
        const std::size_t last = got - sizeof(needle);
        for (std::size_t off = 0; off <= last; off += 4) {
            if (std::memcmp(block + off, needle, sizeof(needle)) != 0) continue;
            const unsigned long long guidAddr = base + off;
            if (guidAddr < kUuidInModule) continue;
            candidates.push_back(guidAddr - kUuidInModule);
        }
    });

    if (candidates.empty()) {
        logf("mods: the base module's uuid is nowhere in memory yet");
        return false;
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()),
                     candidates.end());

    // Pass two: array headers whose buffer is one of them. One scan for all
    // candidates rather than one scan each -- the latter took over a minute,
    // which put it ahead of the module list being built.
    std::vector<Header> headers;
    scan_memory([&](unsigned long long base, unsigned char const* block,
                    std::size_t got) {
        for (std::size_t off = 0; off + kArrayHeader <= got; off += 8) {
            std::uint64_t word = 0;
            std::memcpy(&word, block + off, sizeof(word));
            if (!std::binary_search(candidates.begin(), candidates.end(),
                                    word)) {
                continue;
            }

            std::uint32_t capacity = 0;
            std::uint32_t size = 0;
            std::memcpy(&capacity, block + off + 8, sizeof(capacity));
            std::memcpy(&size, block + off + 12, sizeof(size));
            if (size == 0 || size > 4096 || size > capacity) continue;

            auto const* buffer = (void const*)(std::uintptr_t)word;
            const std::size_t stride = derive_stride(buffer, size);
            if (stride == 0) continue;

            headers.push_back(Header{base + off, Modules{buffer, size,
                                                         stride}});
        }
    });

    if (headers.empty()) {
        logf("mods: the base module's uuid appeared at %zu places but none of "
             "them sat in a module array yet", candidates.size());
        return false;
    }

    // A header is the load order if the rest of ModManager reads correctly
    // around it: BaseModule a module back, AvailableMods an array on. That
    // is what tells LoadOrderedModules apart from AvailableMods itself --
    // both are Array<Module> in the same object, GetLoadOrder means the
    // loaded ones, and picking whichever the scan reached first would
    // silently return the wrong list.
    Manager found{};
    Header const* chosen = nullptr;
    for (Header const& h : headers) {
        Manager m{};
        m.LoadOrder = h.Array;
        m.BaseModule = base_module_before(h.At);
        m.Available = available_after(h.At, h.Array.Stride);
        if (m.BaseModule != nullptr && m.Available.Buffer != nullptr) {
            found = m;
            chosen = &h;
            break;
        }
    }

    // Nothing validated as a whole manager, so nothing is adopted. An
    // earlier version fell back to the first header on the grounds that
    // GetLoadOrder works without the rest; it accepted a four-entry
    // coincidence at stride 336, and because a non-null buffer stops the
    // retry, that wrong answer stuck for the rest of the run and took
    // Ext.Stats' ModId down with it. Waiting is better than guessing.
    if (chosen == nullptr) {
        logf("mods: %zu candidate headers, none with a mod manager around "
             "it yet", headers.size());
        return false;
    }

    state() = found;
    char const* baseUuid = found.BaseModule != nullptr
                               ? uuid_string_at(found.BaseModule)
                               : nullptr;
    logf("mods: %zu modules at %p, stride %zu (header at %#llx, %zu candidate "
         "headers); base module %s, %zu available", chosen->Array.Count,
         chosen->Array.Buffer, chosen->Array.Stride, chosen->At,
         headers.size(), baseUuid != nullptr ? baseUuid : "(not found)",
         found.Available.Count);

    if (std::getenv("BG3LE_DUMP_MODULE") != nullptr) {
        for (Header const& h : headers) {
            logf("mods: header %#llx -> %p, %zu entries, stride %zu%s", h.At,
                 h.Array.Buffer, h.Array.Count, h.Array.Stride,
                 &h == chosen ? " (chosen)" : "");
        }
        for (std::size_t i = 0; i < chosen->Array.Count; ++i) {
            char const* text = uuid_string_at(
                (char const*)chosen->Array.Buffer + i * chosen->Array.Stride);
            logf("mods:   %2zu %s", i, text != nullptr ? text : "(unresolved)");
        }
        dump_module(chosen->Array.Buffer, chosen->Array.Stride);
    }
    return true;
}

// Retried rather than latched: an early failure only means the engine has not
// built the load order yet. The warm thread calls this on a timer, so a cap
// keeps a genuinely absent module list from rescanning memory forever.
bool ready() {
    static int attempts = 0;
    if (state().LoadOrder.Buffer != nullptr) return true;
    if (attempts >= 40) return false;
    ++attempts;
    return search();
}

void const* module_at(Modules const& m, std::size_t index) {
    if (index >= m.Count) return nullptr;
    return (char const*)m.Buffer + index * m.Stride;
}

void const* module_at(std::size_t index) {
    if (!ready()) return nullptr;
    return module_at(state().LoadOrder, index);
}

// Storage for the strings a ModInfo points at, one slot per field so a
// filled struct stays wholly readable until the next call.
char const* hold(int slot, std::string const& text) {
    static thread_local std::string slots[12];
    slots[slot] = text;
    return slots[slot].c_str();
}

char const* read_string_field(void const* module, std::size_t offset,
                              int slot) {
    std::string text;
    if (!read_ls_string((char const*)module + offset, &text)) return "";
    return hold(slot, text);
}

char const* read_fixed_field(void const* module, std::size_t offset) {
    std::uint32_t index = 0;
    if (!read_as((char const*)module + offset, &index)) return "";
    char const* text = bg3le_fixed_string(index, nullptr);
    return text != nullptr ? text : "";
}

// Version is one packed uint64 upstream; unpacked here into the four numbers
// the public API reports.
void read_version(void const* module, std::size_t offset,
                  std::uint32_t out[4]) {
    std::uint64_t packed = 0;
    read_as((char const*)module + offset, &packed);
    out[0] = (std::uint32_t)(packed >> 55);
    out[1] = (std::uint32_t)((packed >> 47) & 0xff);
    out[2] = (std::uint32_t)((packed >> 31) & 0xffff);
    out[3] = (std::uint32_t)(packed & 0x7fffffffull);
}

}  // namespace

extern "C" std::size_t bg3le_mods_count() {
    return ready() ? state().LoadOrder.Count : 0;
}

extern "C" std::size_t bg3le_mods_available_count() {
    return ready() ? state().Available.Count : 0;
}

extern "C" void* bg3le_mods_available_at(std::size_t index) {
    if (!ready()) return nullptr;
    return (void*)module_at(state().Available, index);
}

extern "C" char const* bg3le_mods_uuid_at(std::size_t index) {
    void const* module = module_at(index);
    return module != nullptr ? uuid_string_at(module) : nullptr;
}

extern "C" void* bg3le_mods_at(std::size_t index) {
    return (void*)module_at(index);
}

// The module whose uuid matches, or null.
extern "C" void* bg3le_mods_find(char const* uuid) {
    if (uuid == nullptr || !ready()) return nullptr;
    const std::size_t n = state().LoadOrder.Count;
    for (std::size_t i = 0; i < n; ++i) {
        char const* text = bg3le_mods_uuid_at(i);
        if (text != nullptr && std::strcmp(text, uuid) == 0) {
            return (void*)module_at(i);
        }
    }
    return nullptr;
}

// ModManager::BaseModule, which is the campaign rather than the first module
// in load order -- upstream's GetBaseMod returns that member, so this does.
extern "C" void* bg3le_mods_base() {
    if (!ready()) return nullptr;
    return (void*)state().BaseModule;
}

extern "C" bool bg3le_mod_info(void const* module, ModInfo* out) {
    if (module == nullptr || out == nullptr) return false;

    char const* uuid = uuid_string_at(module);
    if (uuid == nullptr) return false;
    out->ModuleUUIDString = uuid;

    out->Name = read_string_field(module, kInfoName, 0);
    out->Directory = read_string_field(module, kInfoDirectory, 1);
    out->Hash = read_string_field(module, kInfoHash, 2);
    out->Author = read_string_field(module, kInfoAuthor, 3);
    out->Description = read_string_field(module, kInfoDescription, 4);

    out->StartLevelName = read_fixed_field(module, kInfoStartLevel);
    out->MenuLevelName = read_fixed_field(module, kInfoMenuLevel);
    out->LobbyLevelName = read_fixed_field(module, kInfoLobbyLevel);
    out->CharacterCreationLevelName =
        read_fixed_field(module, kInfoCharCreationLevel);
    out->PhotoBoothLevelName = read_fixed_field(module, kInfoPhotoBoothLevel);

    read_version(module, kInfoModVersion, out->ModVersion);
    read_version(module, kInfoPublishVersion, out->PublishVersion);

    out->NumPlayers = 0;
    read_as((char const*)module + kInfoNumPlayers, &out->NumPlayers);
    out->FileSize = 0;
    read_as((char const*)module + kInfoFileSize, &out->FileSize);
    out->PublishHandle = 0;
    read_as((char const*)module + kInfoPublishHandle, &out->PublishHandle);
    return true;
}

extern "C" std::size_t bg3le_mod_list_count(void const* module, int list) {
    if (module == nullptr || list < 0 || list > 2) return 0;
    std::uint32_t size = 0;
    if (!read_as((char const*)module + kModuleLists[list] + 12, &size)) {
        return 0;
    }
    return size > 4096 ? 0 : size;
}

extern "C" bool bg3le_mod_list_at(void const* module, int list,
                                  std::size_t index, ModShortDesc* out) {
    if (out == nullptr || index >= bg3le_mod_list_count(module, list)) {
        return false;
    }

    std::uint64_t buffer = 0;
    if (!read_as((char const*)module + kModuleLists[list], &buffer)) {
        return false;
    }
    if (buffer == 0) return false;
    auto const* desc = (char const*)(std::uintptr_t)buffer
                       + index * kDescStride;

    char const* uuid = read_fixed_field(desc, kDescUuidString);
    // A short desc that does not name a module means the stride is wrong;
    // reporting nothing beats reporting the bytes that follow it.
    if (std::strlen(uuid) != 36) return false;
    out->ModuleUUIDString = uuid;

    out->Name = read_string_field(desc, kDescName, 5);
    out->Folder = read_string_field(desc, kDescFolder, 6);
    out->Hash = read_string_field(desc, kDescHash, 7);
    read_version(desc, kDescModVersion, out->ModVersion);
    read_version(desc, kDescPublishVersion, out->PublishVersion);
    out->PublishHandle = 0;
    read_as(desc + kDescPublishHandle, &out->PublishHandle);
    return true;
}

}  // namespace bg3le
