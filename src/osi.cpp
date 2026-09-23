#include <algorithm>
#include "osi.h"

#include <cctype>
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstring>
#include <link.h>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

#include "log.h"
#include "mem.h"

namespace bg3le::osi {
namespace {

using Thunk6 = long (*)(long, long, long, long, long, long);

Thunk6 g_call = nullptr;
Thunk6 g_query = nullptr;

// Descriptors are 64 bytes in the engine's pool; 128 leaves slack. Max
// observed arity is 11.
constexpr std::size_t kNodeSize = 128;
constexpr std::size_t kMaxParams = 16;

template <typename T>
T sym(const char* name) {
    return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name));
}

struct Accessors {
    void (*set_type)(void*, unsigned short) = nullptr;
    void (*set_integer)(void*, int) = nullptr;
    void (*set_integer64)(void*, long) = nullptr;
    void (*set_float)(void*, float) = nullptr;
    void (*set_string)(void*, const char*) = nullptr;
    int (*get_integer)(const void*) = nullptr;
    long (*get_integer64)(const void*) = nullptr;
    float (*get_float)(const void*) = nullptr;
    const char* (*get_string)(const void*) = nullptr;

    bool ok() const {
        return set_type && set_integer && set_integer64 && set_float &&
               set_string && get_integer && get_integer64 && get_float &&
               get_string;
    }
};

const Accessors& accessors() {
    static Accessors a = [] {
        Accessors r;
        r.set_type = sym<decltype(r.set_type)>(
            "_ZN16COsiArgumentDesc7SetTypeE13TOsiValueType");
        r.set_integer = sym<decltype(r.set_integer)>(
            "_ZN16COsiArgumentDesc10SetIntegerEi");
        r.set_integer64 = sym<decltype(r.set_integer64)>(
            "_ZN16COsiArgumentDesc12SetInteger64El");
        r.set_float = sym<decltype(r.set_float)>("_ZN16COsiArgumentDesc8SetFloatEf");
        r.set_string = sym<decltype(r.set_string)>(
            "_ZN16COsiArgumentDesc12SetAnyStringEPKc");
        r.get_integer = sym<decltype(r.get_integer)>(
            "_ZNK16COsiArgumentDesc10GetIntegerEv");
        r.get_integer64 = sym<decltype(r.get_integer64)>(
            "_ZNK16COsiArgumentDesc12GetInteger64Ev");
        r.get_float = sym<decltype(r.get_float)>("_ZNK16COsiArgumentDesc8GetFloatEv");
        r.get_string = sym<decltype(r.get_string)>(
            "_ZNK16COsiArgumentDesc12GetAnyStringEv");
        return r;
    }();
    return a;
}

// Types 6+ are story enums; they are carried as strings or integers depending
// on what the caller supplied.
unsigned short wire_type(std::uint8_t declared, const Value& v) {
    if (declared >= 6) return v.type == kString ? kGuidString : kInteger;
    return declared;
}

void write(void* node, unsigned short type, const Value& v) {
    const Accessors& a = accessors();
    a.set_type(node, type);  // must precede any setter; it asserts on type
    switch (type) {
        case kInteger: a.set_integer(node, static_cast<int>(v.integer)); break;
        case kInteger64: a.set_integer64(node, static_cast<long>(v.integer)); break;
        case kReal: a.set_float(node, static_cast<float>(v.real)); break;
        case kString:
        case kGuidString: a.set_string(node, v.text.c_str()); break;
        default: break;
    }
}

Value read(const void* node, unsigned short type) {
    const Accessors& a = accessors();
    Value v;
    v.type = type;
    switch (type) {
        case kInteger: v.integer = a.get_integer(node); break;
        case kInteger64: v.integer = a.get_integer64(node); break;
        case kReal: v.real = a.get_float(node); break;
        case kString:
        case kGuidString: {
            const char* s = a.get_string(node);
            if (s != nullptr) v.text = s;
            break;
        }
        default:
            // Story enums come back as whichever representation they use.
            v.integer = a.get_integer(node);
            break;
    }
    return v;
}

}  // namespace

namespace {

// Osiris keeps its function database in a hash of red-black trees reachable
// from a static in libOsiris. The layouts below are bg3se's, which were
// reversed against the Windows build -- the walk validates itself against the
// names we already enumerated rather than trusting them.
//
//   [libOsiris + kFunctionDbHolder] -> holder
//   holder + 0x10                   -> TypeDb::HashSlot[1023], stride 0x18
//   HashSlot + 0x00                 -> item count (NOT a pointer)
//   HashSlot + 0x08                 -> TMap root node
//   TMapNode: Left +00, Parent +08, Right +10, Color +18, IsRoot +19,
//             Key(OsiString) +20, Value +38
//   OsiFunctionDef + 0x18           -> FunctionSignature*
//   FunctionSignature + 0x08        -> const char* Name
//   FunctionSignature + 0x18/+0x20  -> out-param bitmask / byte count
constexpr std::uintptr_t kFunctionDbHolder = 0x119d50;

// Inside OsiFunctionDef (bg3se's GameDefinitions/Osiris.h): VMT, three
// uint32s, the signature pointer at +0x18, a NodeRef, then FunctionType,
// Key[4] and OsiFunctionId. The NodeRef's width decides where Type and
// Key land, so both candidates are tried and the one whose key
// reproduces the engine's own function ids wins -- +0x24 does, for all
// 324 functions both sources list, which puts the node id at +0x20.
constexpr std::uintptr_t kTypeCandidates[] = {0x24, 0x28};

// bg3se's OsirisFunctionHandle: the handle the dispatch handlers take is
// built from the key, and how depends on the function's type.
std::uint32_t function_handle(std::uint32_t type, std::uint32_t part2,
                              std::uint32_t functionId,
                              std::uint32_t part4) {
    std::uint32_t handle = (type & 7) | (part4 << 31);
    if (type < 4) {
        handle |= (functionId & 0x1ffffff) << 3;
    } else {
        handle |= ((functionId & 0x1ffff) << 3) | ((part2 & 0xff) << 20);
    }
    return handle;
}

constexpr std::size_t kBuckets = 1023;
constexpr std::size_t kSlotStride = 0x18;

int find_osiris(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name == nullptr) return 0;
    if (std::strstr(info->dlpi_name, "libOsiris.so") == nullptr) return 0;
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
}

// These offsets are unverified until the walk validates itself, so every
// dereference goes through the fault-tolerant reader: a wrong offset yields
// a failed read instead of taking the game down.
template <typename T>
bool peek(std::uintptr_t addr, T* out) {
    if (addr < 0x1000) return false;
    return safe_read(reinterpret_cast<const void*>(addr), out, sizeof(T));
}

// One object, read in a single call and then parsed locally.
//
// Every field of these structures used to cost a process_vm_readv, and
// the walk touches four thousand of them: the signature walk was 1.75s
// of the level load, nearly all of it syscall overhead. A block read per
// object keeps the fault tolerance -- a wrong offset still yields a
// failed read rather than a segfault -- at a tenth of the calls.
template <std::size_t N>
struct Block {
    std::uintptr_t Base = 0;
    unsigned char Bytes[N] = {};
    bool Ok = false;

    explicit Block(std::uintptr_t base) : Base(base) {
        if (base < 0x1000) return;
        Ok = safe_read(reinterpret_cast<void const*>(base), Bytes, N);
    }

    template <typename T>
    T at(std::size_t off) const {
        T value{};
        if (off + sizeof(T) <= N) std::memcpy(&value, Bytes + off, sizeof(T));
        return value;
    }
};

// Bitmask is MSB-first within each byte, as bg3se's isOutParam does.
int count_out_params(std::uintptr_t bits, std::uint32_t bytes) {
    if (bits == 0 || bytes == 0 || bytes > 64) return 0;

    unsigned char mask[64] = {};
    if (!safe_read(reinterpret_cast<void const*>(bits), mask, bytes)) {
        return -1;
    }

    int total = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        total += __builtin_popcount(mask[i]);
    }
    return total;
}

// Type and handle for one Function object, with `at` the offset of the
// FunctionType field.
bool read_type_and_handle(std::uintptr_t def, std::uintptr_t at,
                          std::uint32_t* type, std::uint32_t* handle) {
    const Block<0x40> block(def);
    if (!block.Ok) return false;

    const auto kind = block.at<std::uint32_t>(at);
    if (kind == 0 || kind > 8) return false;

    std::uint32_t key[4] = {};
    for (int i = 0; i < 4; ++i) {
        key[i] = block.at<std::uint32_t>(at + 4 + i * 4);
    }
    *type = kind;
    *handle = function_handle(key[0], key[1], key[2], key[3]);
    return true;
}

// The declared parameter types, from the signature's parameter list.
//
// The list is circular and doubly linked: the header holds head, tail and
// a count, each node holds Next, Prev and the descriptor, and the type is
// the first sixteen bits of the descriptor. The nodes run in reverse
// declaration order, which is why the result is flipped.
std::vector<std::uint8_t> read_param_types(std::uintptr_t list) {
    std::vector<std::uint8_t> types;
    if (list < 0x1000) return types;

    const Block<0x20> header(list);
    if (!header.Ok) return types;
    const auto count = header.at<std::uint64_t>(0x18);
    if (count == 0 || count > 32) return types;

    std::uintptr_t node = header.at<std::uintptr_t>(0x08);
    for (std::uint64_t i = 0; i < count && node >= 0x1000; ++i) {
        const Block<0x18> entry(node);
        if (!entry.Ok) return {};
        types.push_back((std::uint8_t)entry.at<std::uint16_t>(0x10));
        node = entry.at<std::uintptr_t>(0x00);
    }
    if (types.size() != count) return {};

    std::reverse(types.begin(), types.end());
    return types;
}

// libc++ std::string from the twenty-four bytes of it already read. Only
// the long form needs another read, and most keys are short.
bool parse_osi_string(unsigned char const* bytes, std::string* out) {
    const std::uint8_t last = bytes[23];
    if ((last & 0x80) == 0) {
        const std::size_t len = last;
        if (len > 22) return false;
        out->assign(reinterpret_cast<char const*>(bytes), len);
        return true;
    }

    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    std::memcpy(&data, bytes + 0x00, sizeof(data));
    std::memcpy(&size, bytes + 0x08, sizeof(size));
    if (data < 0x1000 || size == 0 || size > 512) return false;

    std::vector<char> buf(size + 1, 0);
    if (!safe_read(reinterpret_cast<void const*>(data), buf.data(), size)) {
        return false;
    }
    out->assign(buf.data(), size);
    return true;
}


// What the walk keeps about one function in Osiris' own database.
struct DbEntry {
    int Outs = -1;
    std::uintptr_t Def = 0;  // the Function object behind the tree node
    std::vector<std::uint8_t> Types;
};

std::size_t g_visited = 0;

void visit_tree(std::uintptr_t node,
                std::unordered_map<std::string, DbEntry>* out,
                int depth, std::unordered_set<std::uintptr_t>* seen) {
    // Guard against cycles as well as depth: if a link turns out to be a
    // parent pointer rather than a child, this must not spin.
    // No IsRoot check: bg3se's layout puts that flag at +0x19, but pruning on
    // it here discarded live subtrees -- a Lua walk without the check reached
    // nodes this one reported as absent. Null links, the visited set and the
    // depth bound are sufficient termination.
    if (node < 0x1000 || depth > 128) return;
    if (!seen->insert(node).second) return;
    ++g_visited;

    // One read for the whole tree node: the links, the key string and the
    // pointer to the function.
    const Block<0x40> entry(node);
    if (!entry.Ok) return;

    const auto left = entry.at<std::uintptr_t>(0x00);
    const auto right = entry.at<std::uintptr_t>(0x10);
    const auto def = entry.at<std::uintptr_t>(0x38);

    // Key on the tree's own key ("Name/Arity"), not the bare signature name:
    // Osiris overloads by arity, so 1303 functions collapse onto far fewer
    // names and most matches are lost.
    if (def >= 0x1000) {
        const Block<0x28> function(def);
        const auto signature = function.Ok
                                   ? function.at<std::uintptr_t>(0x18)
                                   : 0;
        if (signature >= 0x1000) {
            const Block<0x28> sig(signature);
            if (sig.Ok) {
                const int outs = count_out_params(
                    sig.at<std::uintptr_t>(0x18),
                    sig.at<std::uint32_t>(0x20));
                std::string key;
                if (outs >= 0 && parse_osi_string(entry.Bytes + 0x20, &key)
                    && !key.empty()) {
                    (*out)[key] = DbEntry{
                        outs, def,
                        read_param_types(sig.at<std::uintptr_t>(0x10))};
                }
            }
        }
    }

    visit_tree(left, out, depth + 1, seen);
    visit_tree(right, out, depth + 1, seen);
}

}  // namespace

// The last walk's results, so the story functions can be taken from it
// without walking the tree a second time.
std::unordered_map<std::string, DbEntry>& database() {
    static std::unordered_map<std::string, DbEntry> entries;
    return entries;
}

// Osiris' node list, which is what a story-defined function is actually
// reached through: a procedure is run by inserting a tuple into its node,
// not by calling the engine's dispatch.
//
// OsirisStaticGlobals is a run of pointer slots in libOsiris' data, and
// the function database is one of them -- the one this file already
// uses. NodeDb is five slots further on, but rather than trust that, the
// window around it is searched for something shaped like
// TypedDb<Node> = { uint32 Size; std::vector<Node*> }: the vector's
// extent has to match the size, and the elements have to be objects with
// a vtable inside libOsiris.
struct NodeList {
    std::uintptr_t First = 0;  // the vector's first element
    std::uint32_t Count = 0;
};

NodeList find_node_db(std::uintptr_t base) {
    for (std::intptr_t delta = -0x80; delta <= 0x80; delta += 8) {
        std::uintptr_t db = 0;
        if (!peek(base + kFunctionDbHolder + delta, &db) || db < 0x1000) {
            continue;
        }

        std::uint32_t size = 0;
        std::uintptr_t begin = 0;
        std::uintptr_t end = 0;
        if (!peek(db + 0x00, &size) || size < 16 || size > (1u << 22)) {
            continue;
        }
        if (!peek(db + 0x08, &begin) || !peek(db + 0x10, &end)) continue;
        if (begin < 0x1000 || end <= begin) continue;
        if ((end - begin) / 8 != size) continue;

        // Elements are objects, and an object here starts with a vtable
        // in the library that defines it.
        int checked = 0;
        for (std::uint32_t i = 1; i < size && checked < 4; ++i) {
            std::uintptr_t element = 0;
            if (!peek(begin + i * 8, &element) || element < 0x1000) continue;
            std::uintptr_t vmt = 0;
            if (!peek(element, &vmt) || vmt < 0x1000) return NodeList{};
            ++checked;
        }
        if (checked == 0) continue;

        logf("osiris: node list at libOsiris+%#lx holds %u nodes",
             (unsigned long)(kFunctionDbHolder + delta), size);
        return NodeList{begin, size};
    }
    logf("osiris: no node list found near the function database");
    return NodeList{};
}

// Which offset holds FunctionType, decided once by agreement with the
// engine's mapping.
std::uintptr_t& type_offset() {
    static std::uintptr_t at = 0;
    return at;
}

// Where the signature cache lives, alongside the static-pointer cache.
std::string cache_path(char const* story) {
    char const* home = std::getenv("HOME");
    if (home == nullptr || story == nullptr || story[0] == '\0') return {};

    std::string safe;
    for (char const* at = story; *at != '\0'; ++at) {
        safe += (std::isalnum((unsigned char)*at) != 0) ? *at : '-';
    }
    return std::string(home) + "/.local/share/bg3le/osiris-" + safe + ".txt";
}

// name/arity outs type,type,...
bool load_cached_signatures(char const* story,
                            std::vector<Function>* functions,
                            std::size_t* applied) {
    const std::string path = cache_path(story);
    if (path.empty()) return false;

    std::FILE* f = std::fopen(path.c_str(), "r");
    if (f == nullptr) return false;

    std::unordered_map<std::string, DbEntry> loaded;
    char line[1024];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        char key[512] = {};
        int outs = 0;
        char types[256] = {};
        const int got = std::sscanf(line, "%511s %d %255s", key, &outs, types);
        if (got < 2) continue;

        DbEntry entry;
        entry.Outs = outs;
        if (got == 3) {
            for (char* at = std::strtok(types, ","); at != nullptr;
                 at = std::strtok(nullptr, ",")) {
                entry.Types.push_back((std::uint8_t)std::strtoul(at, nullptr, 10));
            }
        }
        loaded.emplace(key, std::move(entry));
    }
    std::fclose(f);
    if (loaded.empty()) return false;

    std::size_t hits = 0;
    for (Function& fn : *functions) {
        auto it = loaded.find(fn.name + "/" + std::to_string(fn.params.size()));
        if (it == loaded.end()) continue;
        fn.out_params = it->second.Outs;
        ++hits;
    }
    if (hits == 0) return false;

    database() = std::move(loaded);
    *applied = hits;
    logf("osiris: %zu signatures from %s, no walk needed", database().size(),
         path.c_str());
    return true;
}

void save_cached_signatures(char const* story) {
    const std::string path = cache_path(story);
    if (path.empty()) return;

    const std::size_t slash = path.rfind('/');
    if (slash != std::string::npos) {
        ::mkdir(path.substr(0, slash).c_str(), 0755);
    }

    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return;
    for (auto const& entry : database()) {
        std::fprintf(f, "%s %d", entry.first.c_str(), entry.second.Outs);
        for (std::size_t i = 0; i < entry.second.Types.size(); ++i) {
            std::fprintf(f, "%s%u", i == 0 ? " " : ",",
                         (unsigned)entry.second.Types[i]);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
    logf("osiris: wrote %zu signatures to %s", database().size(),
         path.c_str());
}

std::size_t load_out_param_counts(std::vector<Function>* functions,
                                  char const* story) {
    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    if (base == 0) {
        logf("osiris: libOsiris.so not found; cannot read signatures");
        return 0;
    }

    // The node list is wanted either way, and finding it is a handful of
    // reads rather than a walk.
    find_node_db(base);

    std::size_t cached = 0;
    if (load_cached_signatures(story, functions, &cached)) return cached;

    std::uintptr_t holder = 0;
    if (!peek(base + kFunctionDbHolder, &holder) || holder < 0x1000) {
        logf("osiris: function db holder unreadable");
        return 0;
    }

    // One-shot structural dump: the walk found nothing, so look at what is
    // actually there instead of guessing another offset.
    if (std::getenv("BG3LE_DUMP_DB") != nullptr) {
        logf("db: libOsiris base 0x%lx, holder 0x%lx", (unsigned long)base,
             (unsigned long)holder);

        std::uint32_t a = 0, b = 0;
        peek(holder + 0x5fe8, &a);
        peek(holder + 0xc018, &b);
        logf("db: counts at +0x5fe8=%u +0xc018=%u (1303 would confirm the holder)",
             a, b);

        int shown = 0;
        for (std::size_t i = 0; i < kBuckets && shown < 4; ++i) {
            std::uintptr_t w[3] = {};
            const std::uintptr_t slot = holder + 0x10 + i * kSlotStride;
            if (!peek(slot + 0x00, &w[0]) || w[0] == 0) continue;
            peek(slot + 0x08, &w[1]);
            peek(slot + 0x10, &w[2]);
            logf("db: bucket[%zu] @0x%lx = %016lx %016lx %016lx", i,
                 (unsigned long)slot, (unsigned long)w[0], (unsigned long)w[1],
                 (unsigned long)w[2]);

            for (int which = 0; which < 2; ++which) {
                const std::uintptr_t node = which == 0 ? w[0] : w[1];
                if (node < 0x1000) continue;
                std::uintptr_t q[10] = {};
                for (int k = 0; k < 10; ++k) peek(node + k * 8, &q[k]);
                logf("db:   node%d @0x%lx", which, (unsigned long)node);
                for (int k = 0; k < 10; ++k) {
                    char text[96];
                    const bool str = q[k] >= 0x1000 &&
                        safe_cstr(reinterpret_cast<const void*>(q[k]), text,
                                  sizeof(text)) && text[0] >= 0x20 && text[0] < 0x7f;
                    logf("db:     +%02d = %016lx%s%s", k * 8, (unsigned long)q[k],
                         str ? "  -> " : "", str ? text : "");
                }
            }
            ++shown;
        }
    }

    std::unordered_map<std::string, DbEntry>& by_name = database();
    by_name.clear();
    g_visited = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        std::uintptr_t root = 0;
        if (!peek(holder + 0x10 + i * kSlotStride + 0x08, &root)) continue;
        std::unordered_set<std::uintptr_t> seen;
        visit_tree(root, &by_name, 0, &seen);
    }

    // Functions the story never references have no entry here; those keep the
    // caller-decides fallback. bg3se has the same limitation -- it reports
    // "Attempted to call an unbound Osiris function" for them.
    std::size_t applied = 0;
    for (Function& fn : *functions) {
        auto it = by_name.find(fn.name + "/" + std::to_string(fn.params.size()));
        if (it != by_name.end()) {
            fn.out_params = it->second.Outs;
            ++applied;
        }
    }

    // Where FunctionType and the key sit inside the Function object.
    // OsiFunctionDef puts a NodeRef between the signature pointer and
    // them, and its width decides the offset, so both candidates are
    // tried and the one whose key reproduces the engine's own function
    // ids wins. Story-defined functions -- the procedures and user
    // queries mods call -- are in this database but not in the engine's
    // mapping, and that handle is the only way to reach them.
    std::uintptr_t bestAt = 0;
    std::size_t bestHits = 0;
    for (std::uintptr_t at : kTypeCandidates) {
        std::size_t hits = 0;
        for (Function const& fn : *functions) {
            auto it = by_name.find(fn.name + "/"
                                   + std::to_string(fn.params.size()));
            if (it == by_name.end() || it->second.Def == 0) continue;
            std::uint32_t type = 0;
            std::uint32_t handle = 0;
            if (!read_type_and_handle(it->second.Def, at, &type, &handle)) {
                continue;
            }
            if (handle == fn.id) ++hits;
        }
        logf("osiris: FunctionType at +%#lx reproduces %zu of %zu ids",
             (unsigned long)at, hits, applied);
        if (hits > bestHits) {
            bestHits = hits;
            bestAt = at;
        }
    }
    type_offset() = bestHits > applied / 2 ? bestAt : 0;
    save_cached_signatures(story);
    // One known signature, dumped, so the parameter type list can be read
    // off rather than guessed at. BG3LE_DUMP_SIG=1.
    if (std::getenv("BG3LE_DUMP_SIG") != nullptr) {
        for (Function const& fn : *functions) {
            if (fn.params.size() < 2) continue;
            // One with mixed parameter types, so the decoding can be
            // checked rather than merely fitted.
            bool mixed = false;
            for (std::uint8_t t : fn.params) {
                if (t != fn.params[0]) mixed = true;
            }
            if (!mixed) continue;
            auto it = by_name.find(fn.name + "/"
                                   + std::to_string(fn.params.size()));
            if (it == by_name.end() || it->second.Def == 0) continue;

            std::uintptr_t signature = 0;
            if (!peek(it->second.Def + 0x18, &signature)) continue;
            std::string types;
            for (std::uint8_t t : fn.params) {
                types += std::to_string((int)t) + " ";
            }
            logf("sig: %s/%zu id=%u def=0x%lx signature=0x%lx types: %s",
                 fn.name.c_str(), fn.params.size(), fn.id,
                 (unsigned long)it->second.Def, (unsigned long)signature,
                 types.c_str());
            for (std::size_t off = 0; off < 0x40; off += 8) {
                std::uintptr_t word = 0;
                if (!peek(signature + off, &word)) break;
                logf("sig:   +%02zx = %016lx", off, (unsigned long)word);
            }
            // The parameter list at +0x10: its own words, then the chain.
            std::uintptr_t list = 0;
            if (peek(signature + 0x10, &list) && list >= 0x1000) {
                for (std::size_t off = 0; off < 0x28; off += 8) {
                    std::uintptr_t word = 0;
                    if (!peek(list + off, &word)) break;
                    logf("sig: list+%02zx = %016lx", off,
                         (unsigned long)word);
                }
                std::uintptr_t node = 0;
                peek(list + 0x08, &node);
                for (int step = 0; step < 4 && node >= 0x1000; ++step) {
                    std::uintptr_t words[5] = {};
                    for (int k = 0; k < 5; ++k) peek(node + k * 8, &words[k]);
                    logf("sig: node%d @0x%lx = %016lx %016lx %016lx %016lx "
                         "%016lx", step, (unsigned long)node,
                         (unsigned long)words[0], (unsigned long)words[1],
                         (unsigned long)words[2], (unsigned long)words[3],
                         (unsigned long)words[4]);
                    node = words[0];
                }
            }

            // And the first words behind each pointer-looking field.
            for (std::size_t off = 0; off < 0x40; off += 8) {
                std::uintptr_t word = 0;
                if (!peek(signature + off, &word)) break;
                if (word < 0x10000 || word > 0x800000000000ull) continue;
                std::uintptr_t inner[4] = {};
                for (int k = 0; k < 4; ++k) peek(word + k * 8, &inner[k]);
                logf("sig:   [+%02zx] -> %016lx %016lx %016lx %016lx", off,
                     (unsigned long)inner[0], (unsigned long)inner[1],
                     (unsigned long)inner[2], (unsigned long)inner[3]);
            }
            break;
        }
    }

    // Does the decoded parameter list agree with the mapping the engine
    // already gave us? If it does for every function both sources know,
    // it can be trusted for the ones only the database knows.
    std::size_t typed = 0;
    std::size_t typedWrong = 0;
    for (Function const& fn : *functions) {
        auto it = by_name.find(fn.name + "/"
                               + std::to_string(fn.params.size()));
        if (it == by_name.end() || it->second.Types.empty()) continue;
        if (it->second.Types == fn.params) {
            ++typed;
        } else {
            ++typedWrong;
        }
    }
    logf("osiris: parameter types agree for %zu functions, differ for %zu",
         typed, typedWrong);

    logf("osiris: signature walk visited %zu nodes, found %zu entries, "
         "matched %zu of %zu functions", g_visited, by_name.size(), applied,
         functions->size());
    return applied;
}

std::vector<Function> story_functions(std::vector<Function> const& known) {
    std::unordered_set<std::string> seen;
    for (Function const& fn : known) {
        seen.insert(fn.name + "/" + std::to_string(fn.params.size()));
    }

    std::vector<Function> out;
    std::size_t already = 0;
    std::size_t noId = 0;
    std::size_t kinds[9] = {};
    for (auto const& entry : database()) {
        if (seen.count(entry.first) != 0) {
            ++already;
            continue;
        }
        if (entry.second.Def == 0) continue;

        // "Name/Arity" is the database's key; the arity is checked against
        // the parameter list rather than trusted.
        const std::size_t slash = entry.first.rfind('/');
        if (slash == std::string::npos || slash == 0) continue;
        const std::string name = entry.first.substr(0, slash);
        const std::size_t arity = (std::size_t)std::strtoul(
            entry.first.c_str() + slash + 1, nullptr, 10);
        if (entry.second.Types.size() != arity) continue;

        std::uint32_t type = 0;
        std::uint32_t handle = 0;
        if (type_offset() == 0
            || !read_type_and_handle(entry.second.Def, type_offset(), &type,
                                     &handle)
            || handle == 0) {
            ++noId;
            continue;
        }

        Function fn;
        fn.name = name;
        fn.id = handle;
        fn.params = entry.second.Types;
        fn.out_params = entry.second.Outs;
        kinds[type < 9 ? type : 0]++;
        if (type == kProc && kinds[kProc] <= 3) {
            logf("osiris: procedure %s/%zu handle=%#x", fn.name.c_str(),
                 fn.params.size(), fn.id);
        }
        if (type == kEvent) continue;  // the game raises these
        out.push_back(std::move(fn));
    }
    // The ones with no handle are the story's own: a procedure, a
    // database and a user query carry an empty key, because they are not
    // called through the engine's dispatch at all -- Osiris runs them by
    // inserting a tuple into their node. Reaching those needs the node
    // list, which bg3le does not have yet; until then they are counted
    // rather than bound, so nothing claims to call what it cannot.
    logf("osiris: database holds %zu entries: %zu the engine already maps, "
         "%zu callable through the dispatch, %zu story-defined (no "
         "dispatch handle)",
         database().size(), already, out.size(), noId);
    return out;
}

void set_handlers(void* call, void* query) {
    g_call = reinterpret_cast<Thunk6>(call);
    g_query = reinterpret_cast<Thunk6>(query);
}

bool ready() { return g_call != nullptr && g_query != nullptr && accessors().ok(); }

Status invoke(const Function& fn, const std::vector<Value>& inputs,
              std::vector<Value>* outputs) {
    if (!ready()) return Status::kUnavailable;
    if (fn.kind() == kEvent) return Status::kUnavailable;  // the game raises these
    if (fn.params.size() > kMaxParams) return Status::kUnavailable;
    if (inputs.size() > fn.params.size()) return Status::kUnavailable;

    alignas(16) unsigned char storage[kMaxParams][kNodeSize];
    std::memset(storage, 0, sizeof(storage));

    const std::size_t n = fn.params.size();
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t declared = fn.params[i];
        if (i < inputs.size()) {
            write(storage[i], wire_type(declared, inputs[i]), inputs[i]);
        } else {
            // Trailing parameters are outputs: typed, value left cleared.
            accessors().set_type(storage[i], declared >= 6 ? kGuidString : declared);
        }
        void* nxt = (i + 1 < n) ? static_cast<void*>(storage[i + 1]) : nullptr;
        std::memcpy(storage[i], &nxt, sizeof(nxt));  // NextParam at +00
    }

    Thunk6 handler = fn.is_query() ? g_query : g_call;
    long rc = handler(static_cast<long>(fn.id),
                      n > 0 ? reinterpret_cast<long>(storage[0]) : 0, 0, 0, 0, 0);
    if ((rc & 0xff) == 0) return Status::kRejected;

    if (outputs != nullptr) {
        for (std::size_t i = inputs.size(); i < n; ++i) {
            const std::uint8_t declared = fn.params[i];
            outputs->push_back(read(storage[i], declared >= 6 ? kGuidString : declared));
        }
    }
    return Status::kHandled;
}

}  // namespace bg3le::osi
