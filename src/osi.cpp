#include "osi.h"

#include <dlfcn.h>

#include <cstring>

#include "log.h"

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
