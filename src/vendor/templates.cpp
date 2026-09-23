// Root templates, which is what Ext.Template reads.
//
// The template managers have no symbol and hang off nothing bg3le holds,
// so the templates are found directly. That is viable because a
// GameObjectTemplate is unusually self-identifying: past its vtable it
// carries its own Id, TemplateName and ParentTemplateId as FixedStrings
// and its Name as a Larian string, and an Id is always a 36-character
// GUID. Four independent checks on the same object is not something other
// data satisfies by accident.
//
// Deliberately not a fingerprint over a container, which is how the first
// attempt at the translated strings went wrong: here every candidate is
// validated against its own contents before it is kept.

#include <stdafx.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <unistd.h>

#include "ls_string.h"

#include "../log.h"
#include "../mem.h"

extern "C" bool bg3le_scannable_region(char const* line,
                                       unsigned long long* from,
                                       unsigned long long* to);

namespace bg3le {

extern "C" char const* bg3le_fixed_string(std::uint32_t index,
                                          std::uint32_t* length);

namespace {

// Within GameObjectTemplate, past the vtable and the tag container.
constexpr std::size_t kId = 16;
constexpr std::size_t kTemplateName = 20;
constexpr std::size_t kParentTemplateId = 24;
constexpr std::size_t kName = 32;          // a Larian string

constexpr std::uint32_t kNullFixedString = 0xffffffffu;
constexpr std::size_t kGuidLength = 36;

struct Found {
    std::uint64_t Address{0};
    std::string Type;       // "character", "item", ... or empty
};

struct Templates {
    bool Built{false};
    std::unordered_map<std::string, Found> ById;
    std::vector<std::string> Order;
    std::unordered_map<std::uint64_t, std::size_t> ByVtable;
};

Templates& state() {
    static Templates t;
    return t;
}

template <class T>
bool read_as(void const* addr, T* out) {
    return safe_read(addr, out, sizeof(T));
}

bool resolves(std::uint32_t index, std::size_t* lengthOut) {
    if (index == 0 || index == kNullFixedString) return false;
    char const* text = bg3le_fixed_string(index, nullptr);
    if (text == nullptr) return false;
    *lengthOut = std::strlen(text);
    return true;
}

// Whether the object at this address reads as a GameObjectTemplate.
bool is_template(void const* at, std::string* idOut, std::uint64_t* vtable) {
    std::uint64_t vmt = 0;
    if (!read_as(at, &vmt) || vmt < 0x1000) return false;

    std::uint32_t id = 0;
    std::uint32_t templateName = 0;
    std::uint32_t parent = 0;
    if (!read_as((char const*)at + kId, &id)
        || !read_as((char const*)at + kTemplateName, &templateName)
        || !read_as((char const*)at + kParentTemplateId, &parent)) {
        return false;
    }

    // The Id is a GUID.
    std::size_t length = 0;
    if (!resolves(id, &length) || length != kGuidLength) return false;

    // TemplateName is a name, not a GUID, and never empty.
    if (!resolves(templateName, &length) || length == 0) return false;

    // ParentTemplateId is either a GUID or unset.
    if (parent != 0 && parent != kNullFixedString) {
        if (!resolves(parent, &length) || length != kGuidLength) return false;
    }

    // And Name reads as a string.
    std::string name;
    if (!read_ls_string((char const*)at + kName, &name)) return false;

    *idOut = bg3le_fixed_string(id, nullptr);
    *vtable = vmt;
    return true;
}

// A template's type name, read rather than called.
//
// bg3se asks GetType(), a virtual. Calling one blind is how you run a
// destructor by accident, so the slot is decoded instead: on this build
// every template's slot four is
//
//     48 8d 05 <disp32>    lea rax, [rip+disp]
//     c3                   ret
//
// which hands back the address of a per-class static FixedString. The
// displacement is right there in the code, so the string can simply be
// read. The pattern is checked exactly, and anything else yields nothing
// rather than a guess.
constexpr std::size_t kTypeGetterSlot = 4;

char const* type_name_of(std::uint64_t vtable) {
    std::uint64_t fn = 0;
    if (!read_as((char const*)(std::uintptr_t)vtable
                     + kTypeGetterSlot * 8, &fn)) {
        return nullptr;
    }

    unsigned char code[8] = {};
    if (!safe_read((void const*)(std::uintptr_t)fn, code, sizeof(code))) {
        return nullptr;
    }
    if (code[0] != 0x48 || code[1] != 0x8d || code[2] != 0x05
        || code[7] != 0xc3) {
        return nullptr;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, code + 3, sizeof(displacement));
    const auto at = (std::uint64_t)((std::int64_t)fn + 7 + displacement);

    std::uint32_t index = 0;
    if (!read_as((void const*)(std::uintptr_t)at, &index)) return nullptr;
    if (index == 0 || index == kNullFixedString) return nullptr;
    return bg3le_fixed_string(index, nullptr);
}

// Under BG3LE_DUMP_TEMPLATES: the first bytes of each vtable slot.
//
// bg3se asks a template its type through GetType(), a virtual. Calling one
// blind is how you run a destructor by accident, so the slots are read as
// data instead: a getter that hands back the address of a member compiles
// to a two-instruction body, and its displacement says which member --
// which can then simply be read.
void dump_vtable(std::uint64_t vtable) {
    logf("tmpldump: vtable %#llx", (unsigned long long)vtable);
    for (int slot = 0; slot < 10; ++slot) {
        std::uint64_t fn = 0;
        if (!read_as((char const*)(std::uintptr_t)vtable
                         + (std::size_t)slot * 8, &fn)) {
            break;
        }
        unsigned char code[12] = {};
        if (safe_read_some((void const*)(std::uintptr_t)fn, code,
                           sizeof(code)) == 0) {
            continue;
        }
        logf("tmpldump:   slot %2d -> %#llx  %02x %02x %02x %02x %02x %02x "
             "%02x %02x", slot, (unsigned long long)fn, code[0], code[1],
             code[2], code[3], code[4], code[5], code[6], code[7]);
    }
}

// The executable's own mapping, so a candidate's vtable pointer can be
// tested without a syscall. A code pointer into the image is rare in data,
// which is what makes this cheap enough to apply per eight bytes.
bool image_range(unsigned long long* from, unsigned long long* to) {
    static unsigned long long low = 0;
    static unsigned long long high = 0;
    if (high != 0) {
        *from = low;
        *to = high;
        return true;
    }

    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    char line[1024];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        if (std::strstr(line, exe) == nullptr) continue;
        unsigned long long a = 0;
        unsigned long long b = 0;
        if (std::sscanf(line, "%llx-%llx", &a, &b) != 2) continue;
        if (low == 0 || a < low) low = a;
        if (b > high) high = b;
    }
    std::fclose(maps);

    *from = low;
    *to = high;
    return high != 0;
}

bool build() {
    Templates found{};

    unsigned long long imageFrom = 0;
    unsigned long long imageTo = 0;
    if (!image_range(&imageFrom, &imageTo)) {
        logf("templates: cannot locate the executable's mapping");
        return false;
    }

    std::FILE* maps = std::fopen("/proc/self/maps", "r");
    if (maps == nullptr) return false;

    constexpr std::size_t kChunk = 1u << 20;
    constexpr std::size_t kObject = 48;
    static std::vector<unsigned char> block;
    block.resize(kChunk + kObject);

    char line[512];
    while (std::fgets(line, sizeof(line), maps) != nullptr) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        if (!bg3le_scannable_region(line, &from, &to)) continue;

        for (unsigned long long base = from; base < to; base += kChunk) {
            std::size_t want = (std::size_t)(to - base);
            if (want > kChunk + kObject) want = kChunk + kObject;
            const std::size_t got =
                safe_read_some((void const*)base, block.data(), want);
            if (got < kObject) continue;

            for (std::size_t off = 0; off + kObject <= got; off += 8) {
                // Every rejection here is from the block already read, not
                // a syscall. Without that the scan calls is_template on
                // almost every slot in a multi-gigabyte address space and
                // takes hours -- the same mistake that once locked the
                // story thread.
                std::uint64_t vmt = 0;
                std::memcpy(&vmt, block.data() + off, sizeof(vmt));
                if (vmt < imageFrom || vmt >= imageTo) continue;
                if ((vmt & 7) != 0) continue;

                std::uint32_t id = 0;
                std::uint32_t templateName = 0;
                std::memcpy(&id, block.data() + off + kId, sizeof(id));
                std::memcpy(&templateName, block.data() + off + kTemplateName,
                            sizeof(templateName));
                if (id == 0 || id == kNullFixedString) continue;
                if (templateName == 0 || templateName == kNullFixedString) {
                    continue;
                }

                std::string key;
                std::uint64_t vtable = 0;
                if (!is_template((void const*)(base + off), &key, &vtable)) {
                    continue;
                }

                // The first one wins: a template can be referenced from
                // several places but only one object is the template.
                char const* type = type_name_of(vtable);
                Found entry{base + off, type != nullptr ? type : ""};
                if (found.ById.emplace(key, entry).second) {
                    ++found.ByVtable[vtable];
                }
            }
        }
    }
    std::fclose(maps);

    if (found.ById.size() < 100) {
        logf("templates: only %zu candidates found; treating that as not "
             "found rather than publishing a partial set",
             found.ById.size());
        return false;
    }

    found.Order.reserve(found.ById.size());
    for (auto const& entry : found.ById) found.Order.push_back(entry.first);

    found.Built = true;
    state() = std::move(found);
    std::size_t typed = 0;
    for (auto const& entry : state().ById) {
        if (!entry.second.Type.empty()) ++typed;
    }
    logf("templates: %zu templates across %zu distinct vtables, %zu with a "
         "type name", state().ById.size(), state().ByVtable.size(), typed);

    if (std::getenv("BG3LE_DUMP_TEMPLATES") != nullptr) {
        std::unordered_map<std::string, std::size_t> byType;
        for (auto const& entry : state().ById) ++byType[entry.second.Type];
        for (auto const& entry : byType) {
            logf("templates: type \"%s\": %zu", entry.first.c_str(),
                 entry.second);
        }
        std::size_t shown = 0;
        for (auto const& entry : state().ByVtable) {
            logf("templates: vtable %#llx holds %zu templates",
                 (unsigned long long)entry.first, entry.second);
            if (shown++ < 2) dump_vtable(entry.first);
        }
    }
    return true;
}

bool ready() {
    if (state().Built) return true;

    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return build();
}

}  // namespace

extern "C" bool bg3le_templates_ready() { return ready(); }

extern "C" std::size_t bg3le_templates_count() {
    return ready() ? state().ById.size() : 0;
}

extern "C" char const* bg3le_templates_id_at(std::size_t index) {
    if (!ready() || index >= state().Order.size()) return nullptr;
    return state().Order[index].c_str();
}

extern "C" void* bg3le_templates_find(char const* id) {
    if (id == nullptr || !ready()) return nullptr;

    auto it = state().ById.find(id);
    if (it == state().ById.end()) return nullptr;
    return (void*)(std::uintptr_t)it->second.Address;
}

// The engine's own name for a template's type: "character", "item" and so
// on, from the class's static FixedString.
extern "C" char const* bg3le_templates_type(char const* id) {
    if (id == nullptr || !ready()) return nullptr;

    auto it = state().ById.find(id);
    if (it == state().ById.end() || it->second.Type.empty()) return nullptr;
    return it->second.Type.c_str();
}

}  // namespace bg3le
