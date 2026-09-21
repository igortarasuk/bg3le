#include "osi.h"

#include <dlfcn.h>

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

// Bitmask is MSB-first within each byte, as bg3se's isOutParam does.
int count_out_params(std::uintptr_t signature) {
    std::uintptr_t bits = 0;
    std::uint32_t bytes = 0;
    if (!peek(signature + 0x18, &bits) || !peek(signature + 0x20, &bytes)) return -1;
    if (bits == 0 || bytes == 0 || bytes > 64) return 0;

    int total = 0;
    for (std::uint32_t i = 0; i < bytes; ++i) {
        std::uint8_t b = 0;
        if (!peek(bits + i, &b)) return -1;
        total += __builtin_popcount(b);
    }
    return total;
}

// libc++ std::string. Short form keeps the data inline with the length in
// the final byte; long form is {pointer, size, capacity|MSB}.
bool read_osi_string(std::uintptr_t str, std::string* out) {
    std::uint8_t last = 0;
    if (!peek(str + 23, &last)) return false;

    if ((last & 0x80) == 0) {
        const std::size_t len = last;
        if (len > 22) return false;
        char buf[24] = {};
        if (!safe_read(reinterpret_cast<const void*>(str), buf, 23)) return false;
        out->assign(buf, len);
        return true;
    }

    std::uintptr_t data = 0;
    std::uint64_t size = 0;
    if (!peek(str + 0x00, &data) || !peek(str + 0x08, &size)) return false;
    if (data < 0x1000 || size == 0 || size > 512) return false;
    std::vector<char> buf(size + 1, 0);
    if (!safe_read(reinterpret_cast<const void*>(data), buf.data(), size)) return false;
    out->assign(buf.data(), size);
    return true;
}

void visit_tree(std::uintptr_t node, std::unordered_map<std::string, int>* out,
                int depth, std::unordered_set<std::uintptr_t>* seen) {
    // Guard against cycles as well as depth: if a link turns out to be a
    // parent pointer rather than a child, this must not spin.
    // No IsRoot check: bg3se's layout puts that flag at +0x19, but pruning on
    // it here discarded live subtrees -- a Lua walk without the check reached
    // nodes this one reported as absent. Null links, the visited set and the
    // depth bound are sufficient termination.
    if (node < 0x1000 || depth > 128) return;
    if (!seen->insert(node).second) return;

    std::uintptr_t left = 0, right = 0, def = 0;
    if (!peek(node + 0x00, &left) || !peek(node + 0x10, &right) ||
        !peek(node + 0x38, &def)) {
        return;
    }

    // Key on the tree's own key ("Name/Arity"), not the bare signature name:
    // Osiris overloads by arity, so 1303 functions collapse onto far fewer
    // names and most matches are lost.
    std::uintptr_t signature = 0;
    if (def >= 0x1000 && peek(def + 0x18, &signature) && signature >= 0x1000) {
        const int outs = count_out_params(signature);
        std::string key;
        if (outs >= 0 && read_osi_string(node + 0x20, &key) && !key.empty()) {
            (*out)[key] = outs;
        }
    }

    visit_tree(left, out, depth + 1, seen);
    visit_tree(right, out, depth + 1, seen);
}

}  // namespace

std::size_t load_out_param_counts(std::vector<Function>* functions) {
    std::uintptr_t base = 0;
    ::dl_iterate_phdr(find_osiris, &base);
    if (base == 0) {
        logf("osiris: libOsiris.so not found; cannot read signatures");
        return 0;
    }

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

    std::unordered_map<std::string, int> by_name;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        std::uintptr_t root = 0;
        if (!peek(holder + 0x10 + i * kSlotStride + 0x08, &root)) continue;
        std::unordered_set<std::uintptr_t> seen;
        visit_tree(root, &by_name, 0, &seen);
    }

    // Diagnose the misses: is the bare name absent entirely, or present with
    // a different arity?
    std::unordered_map<std::string, std::string> arities_by_name;
    for (const auto& kv : by_name) {
        const std::size_t slash = kv.first.rfind('/');
        if (slash == std::string::npos) continue;
        const std::string bare = kv.first.substr(0, slash);
        const std::string ar = kv.first.substr(slash + 1);
        auto& acc = arities_by_name[bare];
        acc += (acc.empty() ? "" : ",") + ar;
    }

    std::size_t applied = 0;
    std::size_t wrong_arity = 0;
    std::size_t absent = 0;
    int shown = 0;
    for (Function& fn : *functions) {
        const std::string key = fn.name + "/" + std::to_string(fn.params.size());
        auto it = by_name.find(key);
        if (it != by_name.end()) {
            fn.out_params = it->second;
            ++applied;
            continue;
        }
        auto alt = arities_by_name.find(fn.name);
        if (alt != arities_by_name.end()) {
            ++wrong_arity;
            if (shown < 8) {
                logf("  miss: %s wants /%zu, db has /%s", fn.name.c_str(),
                     fn.params.size(), alt->second.c_str());
                ++shown;
            }
        } else {
            ++absent;
            if (shown < 8) {
                logf("  miss: %s (/%zu) -- bare name not in db at all",
                     fn.name.c_str(), fn.params.size());
                ++shown;
            }
        }
    }
    logf("osiris: %zu matched, %zu wrong arity, %zu absent", applied, wrong_arity,
         absent);
    logf("osiris: signature walk found %zu entries, matched %zu of %zu functions",
         by_name.size(), applied, functions->size());
    return applied;
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

    Thunk6 handler = fn.kind() == kQuery ? g_query : g_call;
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
