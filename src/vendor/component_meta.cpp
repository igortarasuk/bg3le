// A field table for every class bg3se describes, built from its own generated
// metadata.
//
// bg3se already knows the name, offset and type of every field of every
// component -- 3,071 classes and some 12,500 offset-based fields, in
// GameDefinitions/Generated/PropertyMaps.inl. Hand-writing an accessor per
// component throws all of that away.
//
// Its runtime property maps are not usable here, though. They are keyed by
// FixedString, which is a 32-bit index into the engine's global string table,
// and that table has no symbol and no entry point: ls::FixedString's methods
// are all inlined in the native build, so there is nothing to borrow the way
// the allocator was. The property getters also take a LifetimeHandle and push
// through bg3se's Lua State, which bg3le does not have.
//
// None of that is needed. PropertyMaps.inl is macro-driven, and upstream
// #defines and #undefs the macros around its own include of it, so the same
// file can be included again here with different definitions. The offsets come
// from offsetof and the types from decltype, so this is the compiler's own
// layout knowledge rather than a restatement of it -- the same reason the
// hand-written Health accessors were trustworthy, applied to everything at
// once.
//
// What comes out is a plain table: no FixedString, no Lua, no lifetimes. Field
// names are the string literals from the generated file, so they need no
// interning and outlive everything.
//
// The generated metadata and the definitions it describes are by Norbyte and
// the bg3se contributors (https://github.com/Norbyte/bg3se); only this
// re-expansion of it is ours. With thanks to them.

// stdafx.h first, then the dependency header, exactly as upstream's own
// translation unit orders them; the generated metadata relies on it.
#include <stdafx.h>

#include <Lua/Shared/Proxies/PropertyMapDependencies.h>

#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../component_meta_abi.h"

namespace bg3le {

using namespace bg3se;

// The name of a type, taken from the compiler's own function-name string.
//
// This is how a field's type gets recorded without its field table having to
// be complete at that point. Requiring completeness would mean a field could
// only be described if its type happened to be declared earlier in the
// generated file, which is not how that file is ordered. Instead both a
// class's own name and a field's type name come from here, so they are written
// by the same scheme and compare exactly, and a field type that turns out to
// have no table of its own simply fails to resolve at load.
template <class T>
constexpr std::string_view type_name() {
    std::string_view p = __PRETTY_FUNCTION__;
    const auto start = p.find("T = ") + 4;
    const auto end = p.rfind(']');
    return p.substr(start, end - start);
}

// std::array is how the engine stores the per-ability and per-skill tables,
// so it is worth recognising rather than reporting as unsupported.
template <class T>
struct ArrayTraits {
    static constexpr bool kIsArray = false;
    using Elem = void;
    static constexpr std::size_t kCount = 0;
};

template <class T, std::size_t N>
struct ArrayTraits<std::array<T, N>> {
    static constexpr bool kIsArray = true;
    using Elem = T;
    static constexpr std::size_t kCount = N;
};

// The field kinds bg3le can read and write without interpretation. Enums
// resolve to their underlying integer, which is how the engine stores them and
// how a script wants to see them.
template <class T>
constexpr FieldKind scalar_kind_of() {
    if constexpr (std::is_enum_v<T>) {
        return scalar_kind_of<std::underlying_type_t<T>>();
    }
    else if constexpr (std::is_same_v<T, bool>) return FieldKind::Bool;
    else if constexpr (std::is_same_v<T, float>) return FieldKind::Float;
    else if constexpr (std::is_same_v<T, double>) return FieldKind::Double;
    else if constexpr (std::is_same_v<T, std::int8_t>) return FieldKind::Int8;
    else if constexpr (std::is_same_v<T, std::uint8_t>) return FieldKind::Uint8;
    else if constexpr (std::is_same_v<T, std::int16_t>) return FieldKind::Int16;
    else if constexpr (std::is_same_v<T, std::uint16_t>) return FieldKind::Uint16;
    else if constexpr (std::is_same_v<T, std::int32_t>) return FieldKind::Int32;
    else if constexpr (std::is_same_v<T, std::uint32_t>) return FieldKind::Uint32;
    else if constexpr (std::is_same_v<T, std::int64_t>) return FieldKind::Int64;
    else if constexpr (std::is_same_v<T, std::uint64_t>) return FieldKind::Uint64;
    else if constexpr (std::is_same_v<T, Guid>) return FieldKind::Guid;
    else if constexpr (std::is_same_v<T, EntityHandle>) return FieldKind::Entity;
    else return FieldKind::Unsupported;
}

// An array of scalars is reported as one. A class type that is not a scalar in
// disguise is reported as a struct, and whether it can actually be traversed
// is decided at load by whether its type name resolves to a field table --
// so HashMap and DynamicArray land here too and simply fail to resolve, which
// is the honest answer until they are handled.
template <class T>
constexpr FieldKind kind_of() {
    if constexpr (ArrayTraits<T>::kIsArray) {
        if constexpr (scalar_kind_of<typename ArrayTraits<T>::Elem>()
                      != FieldKind::Unsupported) {
            return FieldKind::ScalarArray;
        } else {
            return FieldKind::Unsupported;
        }
    } else if constexpr (scalar_kind_of<T>() != FieldKind::Unsupported) {
        return scalar_kind_of<T>();
    } else if constexpr (std::is_class_v<T>) {
        return FieldKind::Struct;
    } else {
        return FieldKind::Unsupported;
    }
}

template <class T>
constexpr FieldKind elem_kind_of() {
    if constexpr (ArrayTraits<T>::kIsArray) {
        return scalar_kind_of<typename ArrayTraits<T>::Elem>();
    } else {
        return FieldKind::Unsupported;
    }
}

template <class T>
constexpr std::uint16_t elem_count_of() {
    return (std::uint16_t)ArrayTraits<T>::kCount;
}

// Components carry two names, and both are wanted.
//
// EngineClass is the engine's own name ("eoc::HealthComponent"), which is what
// bg3le's symbol-table registry is keyed by. ComponentName is bg3se's short
// name ("Health"), which is what its Lua API exposes -- so carrying it means
// entity.Health resolves generically instead of through a hardcoded map.
template <class T>
constexpr char const* engine_class_of() {
    if constexpr (IsComponentType<T>) return T::EngineClass;
    else return nullptr;
}

template <class T>
constexpr char const* component_name_of() {
    if constexpr (IsComponentType<T>) return T::ComponentName;
    else return nullptr;
}

struct ClassFields {
    char const* Name;           // the C++ class name, what INHERIT refers to
    char const* ComponentName;  // bg3se's short name, or null
    char const* EngineClass;    // the engine's name, or null
    // The fully qualified type name, written by the same type_name<T>() that
    // writes a field's type name, so a nested field type resolves by an exact
    // compare rather than by guessing at qualification.
    std::string_view TypeName;
    FieldDesc const* Fields;
    // The stride bg3se walks a component page with, so it has to be the
    // engine's real component size.
    std::size_t Size;
};

template <class T>
struct FieldTable;

}  // namespace bg3le

// ---------------------------------------------------------------------------
// The re-expansion.
//
// Only the offset-based macros produce entries. P_FUN, P_GETTER,
// P_FREE_GETTER, P_GETTER_SETTER and P_FALLBACK describe computed properties
// with no storage of their own, so there is no offset to record; P_BITMASK has
// one but needs the bit position too, and is left out until it is needed. All
// of them expand to nothing rather than to a wrong entry.
//
// offsetof on these classes is not standard -- most are not standard-layout --
// but it is what upstream uses to build the same table, and it is how every
// component offset in bg3le has been derived so far.
// ---------------------------------------------------------------------------

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"

#define GENERATING_PROPMAP

#define BEGIN_CLS_TN(cls, typeName, id)                                       \
    namespace bg3le {                                                         \
    template <> struct FieldTable<cls> {                                      \
        using ObjectType = cls;                                               \
        static constexpr char const* kName = #cls;                            \
        static constexpr char const* kComponentName =                         \
            component_name_of<cls>();                                         \
        static constexpr char const* kEngineClass = engine_class_of<cls>();   \
        static constexpr std::string_view kTypeName = type_name<cls>();       \
        static constexpr FieldDesc kFields[] = {

#define BEGIN_CLS(cls, id) BEGIN_CLS_TN(cls, cls, id)

// A class with no offset-based fields would otherwise declare a zero-length
// array, which is not valid C++, so every table is null-terminated.
#define END_CLS()                                                             \
            { nullptr, 0, 0, FieldKind::Unsupported,                          \
              FieldKind::Unsupported, 0, nullptr, 0 },                        \
        };                                                                    \
    };                                                                        \
    }

// Recorded as an entry rather than as a member, so it needs no surgery on the
// initialiser list and so multiple bases work. Resolved by name at load,
// because a class's table may be declared before its base's.
#define INHERIT(base)                                                         \
        { #base, 0, 0, FieldKind::Inherit, FieldKind::Unsupported, 0,         \
          nullptr, 0 },

#define PN(name, prop)                                                        \
        { #name, (std::uint32_t)offsetof(ObjectType, prop),                   \
          (std::uint16_t)sizeof(decltype(ObjectType::prop)),                  \
          kind_of<decltype(ObjectType::prop)>(),                              \
          elem_kind_of<decltype(ObjectType::prop)>(),                         \
          elem_count_of<decltype(ObjectType::prop)>(),                        \
          type_name<decltype(ObjectType::prop)>().data(),                     \
          (std::uint16_t)type_name<decltype(ObjectType::prop)>().size() },

#define P(prop) PN(prop, prop)
#define P_RO(prop) PN(prop, prop)
#define PN_RO(name, prop) PN(name, prop)
#define P_NOTIFY(prop, notify) PN(prop, prop)
// The old name is deliberately not recorded; bg3le has no deprecated names to
// stay compatible with.
#define P_RENAMED(prop, oldName) PN(prop, prop)

#define P_BITMASK(prop)
#define P_BITMASK_GETTER_SETTER(prop, getter, setter)
#define P_GETTER(name, fun)
#define P_FREE_GETTER(name, fun)
#define P_GETTER_SETTER(name, getter, setter)
#define P_FUN(name, fun)
#define P_FALLBACK(getter, setter, next)

#include <GameDefinitions/Generated/PropertyMaps.inl>

#undef GENERATING_PROPMAP
#undef BEGIN_CLS
#undef BEGIN_CLS_TN
#undef END_CLS
#undef INHERIT
#undef P
#undef PN
#undef P_RO
#undef PN_RO
#undef P_NOTIFY
#undef P_RENAMED
#undef P_BITMASK
#undef P_BITMASK_GETTER_SETTER
#undef P_GETTER
#undef P_FREE_GETTER
#undef P_GETTER_SETTER
#undef P_FUN
#undef P_FALLBACK

#pragma clang diagnostic pop

namespace bg3le {
namespace {

template <class T>
inline constexpr ClassFields kClassFields{
    FieldTable<T>::kName,
    FieldTable<T>::kComponentName,
    FieldTable<T>::kEngineClass,
    FieldTable<T>::kTypeName,
    FieldTable<T>::kFields,
    sizeof(T),
};

// Every class table, collected the way upstream collects its own.
constexpr ClassFields const* kAllClasses[] = {
#define DECLARE_CLS(id, ...) &kClassFields<__VA_ARGS__>,
#define DECLARE_CLS_FWD(id, cls) &kClassFields<cls>,
#define DECLARE_CLS_NS_FWD(id, ns, cls) &kClassFields<ns::cls>,
#define DECLARE_CLS_BARE_NS_FWD(id, ns, cls) &kClassFields<::ns::cls>,
#define DECLARE_STRUCT_BARE_NS_FWD(id, ns, cls) &kClassFields<::ns::cls>,
#include <GameDefinitions/Generated/PropertyMapNames.inl>
#undef DECLARE_CLS
#undef DECLARE_CLS_FWD
#undef DECLARE_CLS_NS_FWD
#undef DECLARE_CLS_BARE_NS_FWD
#undef DECLARE_STRUCT_BARE_NS_FWD
};

// Name -> table, built once. Both indexes are wanted: the engine name is what
// the ECS registry gives us, the class name is what INHERIT refers to.
std::unordered_map<std::string_view, ClassFields const*>& by_class_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        m.reserve(std::size(kAllClasses) * 2);
        for (auto const* cls : kAllClasses) m.emplace(cls->Name, cls);
        return m;
    }();
    return map;
}

// Indexed under both names a component has, so callers can use whichever they
// have: the engine name from the ECS registry, or bg3se's short name from a
// script. They cannot collide -- one is namespace-qualified and the other is
// not.
std::unordered_map<std::string_view, ClassFields const*>& by_component_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        for (auto const* cls : kAllClasses) {
            if (cls->EngineClass != nullptr) m.emplace(cls->EngineClass, cls);
            if (cls->ComponentName != nullptr) m.emplace(cls->ComponentName, cls);
        }
        return m;
    }();
    return map;
}

// Walks a class and its bases for a field. Bases contribute at the same offset
// because bg3se's components inherit from empty tag bases, which is also why
// the offsets need no adjustment.
FieldDesc const* find_field(ClassFields const* cls, char const* name,
                            unsigned depth = 0) {
    if (cls == nullptr || depth > 8) return nullptr;

    for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
        if (f->Kind == FieldKind::Inherit) continue;
        if (std::strcmp(f->Name, name) == 0) return f;
    }

    for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
        if (f->Kind != FieldKind::Inherit) continue;
        auto it = by_class_name().find(f->Name);
        if (it == by_class_name().end()) continue;
        if (auto const* found = find_field(it->second, name, depth + 1)) {
            return found;
        }
    }
    return nullptr;
}

// Types by their type_name<T>() spelling, so a Struct field can be resolved to
// the table describing it. Only the types bg3se describes are in here; a field
// whose type it does not describe -- a HashMap, say -- simply misses.
std::unordered_map<std::string_view, ClassFields const*>& by_type_name() {
    static std::unordered_map<std::string_view, ClassFields const*> map = [] {
        std::unordered_map<std::string_view, ClassFields const*> m;
        m.reserve(std::size(kAllClasses));
        for (auto const* cls : kAllClasses) m.emplace(cls->TypeName, cls);
        return m;
    }();
    return map;
}

// The table describing a Struct field's type, or null if bg3se does not
// describe it.
ClassFields const* struct_type_of(FieldDesc const* field) {
    if (field == nullptr || field->Kind != FieldKind::Struct
        || field->TypeName == nullptr) {
        return nullptr;
    }
    auto it = by_type_name().find(
        std::string_view(field->TypeName, field->TypeNameLength));
    return it != by_type_name().end() ? it->second : nullptr;
}

// Resolves a dotted path -- "DiceValues.Amount" -- accumulating the offset of
// each step.
//
// Nesting is expressed as a path rather than by handing out a table for the
// inner struct, so reading a nested field stays a single call and needs no
// object to be kept alive on either side.
FieldDesc const* find_field_path(ClassFields const* cls, char const* path,
                                 std::uint32_t* offset) {
    *offset = 0;
    if (cls == nullptr || path == nullptr) return nullptr;

    std::string_view rest(path);
    FieldDesc const* field = nullptr;

    // Bounded so a pathological path cannot spin; nothing in the metadata
    // nests anywhere near this deep.
    for (unsigned step = 0; step < 16; ++step) {
        const auto dot = rest.find('.');
        const std::string_view segment =
            dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (segment.empty()) return nullptr;

        // find_field takes a NUL-terminated name, and a path segment is not
        // one.
        const std::string name(segment);
        field = find_field(cls, name.c_str());
        if (field == nullptr) return nullptr;
        *offset += field->Offset;

        if (dot == std::string_view::npos) return field;

        cls = struct_type_of(field);
        if (cls == nullptr) return nullptr;  // cannot descend through this
        rest = rest.substr(dot + 1);
    }
    return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// The C interface bg3le uses.
// ---------------------------------------------------------------------------

// Looks a component up by its engine name, as it appears in the ECS registry
// ("eoc::HealthComponent"). Returns an opaque handle, or null if bg3se has no
// metadata for it.
extern "C" void const* bg3le_meta_component(char const* engineName) {
    if (engineName == nullptr) return nullptr;
    auto it = by_component_name().find(engineName);
    return it != by_component_name().end() ? it->second : nullptr;
}

// The declared size of a component, which is the stride bg3se uses to walk a
// component page. Without metadata there is no safe stride, hence 0.
extern "C" std::size_t bg3le_meta_component_size(void const* handle) {
    if (handle == nullptr) return 0;
    return static_cast<ClassFields const*>(handle)->Size;
}

// Resolves a field of a component by name or by dotted path, following base
// classes. Returns false if the path does not resolve, including when it tries
// to descend through a type bg3se does not describe.
extern "C" bool bg3le_meta_field(void const* handle, char const* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount) {
    if (handle == nullptr || name == nullptr) return false;
    std::uint32_t pathOffset = 0;
    auto const* field = find_field_path(
        static_cast<ClassFields const*>(handle), name, &pathOffset);
    if (field == nullptr) return false;
    *offset = pathOffset;
    *size = field->Size;
    // As in bg3le_meta_fields_at: a struct with no table behind it is reported
    // as unsupported, because nothing can be done with it.
    *kind = (field->Kind == FieldKind::Struct && struct_type_of(field) == nullptr)
                ? (std::uint8_t)FieldKind::Unsupported
                : (std::uint8_t)field->Kind;
    *elemKind = (std::uint8_t)field->ElemKind;
    *elemCount = field->ElemCount;
    return true;
}

// Enumerates a component's own fields, base classes included, for listing a
// component from Lua. Returns the number written.
// path may be null or empty for the component itself, or a dotted path to a
// nested struct, so a script can list what an inner struct offers the same way
// it lists a component.
extern "C" std::size_t bg3le_meta_fields_at(void const* handle,
                                            char const* path,
                                            char const** names,
                                            std::uint8_t* kinds,
                                            std::size_t capacity) {
    if (handle == nullptr) return 0;

    auto const* cls = static_cast<ClassFields const*>(handle);
    if (path != nullptr && path[0] != '\0') {
        std::uint32_t offset = 0;
        auto const* field = find_field_path(cls, path, &offset);
        cls = struct_type_of(field);
        if (cls == nullptr) return 0;
    }

    std::size_t n = 0;
    std::vector<ClassFields const*> pending{cls};

    while (!pending.empty() && n < capacity) {
        auto const* cls = pending.back();
        pending.pop_back();
        for (auto const* f = cls->Fields; f->Name != nullptr; ++f) {
            if (f->Kind == FieldKind::Inherit) {
                auto it = by_class_name().find(f->Name);
                if (it != by_class_name().end()) pending.push_back(it->second);
                continue;
            }
            if (n >= capacity) break;
            names[n] = f->Name;
            // A Struct whose type bg3se does not describe cannot be descended
            // into, so it is reported as unsupported rather than as a struct.
            // That keeps the reported kind something a caller can act on.
            kinds[n] = (f->Kind == FieldKind::Struct
                        && struct_type_of(f) == nullptr)
                           ? (std::uint8_t)FieldKind::Unsupported
                           : (std::uint8_t)f->Kind;
            ++n;
        }
    }
    return n;
}

extern "C" std::size_t bg3le_meta_fields(void const* handle,
                                         char const** names,
                                         std::uint8_t* kinds,
                                         std::size_t capacity) {
    return bg3le_meta_fields_at(handle, nullptr, names, kinds, capacity);
}

// How many classes carry metadata, for the startup log.
extern "C" std::size_t bg3le_meta_class_count() { return std::size(kAllClasses); }

extern "C" std::size_t bg3le_meta_component_count() {
    std::size_t n = 0;
    for (auto const* cls : kAllClasses) {
        if (cls->EngineClass != nullptr) ++n;
    }
    return n;
}

// The engine's name for a component, so a caller who looked the component up
// by bg3se's short name can still reach bg3le's symbol-table index, which is
// keyed by the engine name.
extern "C" char const* bg3le_meta_engine_class(void const* handle) {
    if (handle == nullptr) return nullptr;
    return static_cast<ClassFields const*>(handle)->EngineClass;
}

// bg3se's short name, which is what a script writes as entity.<Name>.
extern "C" char const* bg3le_meta_short_name(void const* handle) {
    if (handle == nullptr) return nullptr;
    return static_cast<ClassFields const*>(handle)->ComponentName;
}

}  // namespace bg3le
