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
#include "../log.h"

// From platform_linux.cpp; the self-test below needs an allocator to build an
// array with.
extern "C" bool bg3le_game_allocator_ready();

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

// glm's vectors and quaternions are fixed runs of floats, and they carry
// everything positional in the engine -- Bound.Translate is a glm::vec3, so
// without this the position of anything in the world reads as unsupported.
// They are plain aggregates, so they read exactly as a fixed-extent array
// does.
template <class T>
struct GlmTraits {
    static constexpr bool kIsGlm = false;
    using Elem = void;
    static constexpr std::size_t kCount = 0;
};

template <glm::length_t N, class T, glm::qualifier Q>
struct GlmTraits<glm::vec<N, T, Q>> {
    static constexpr bool kIsGlm = true;
    using Elem = T;
    static constexpr std::size_t kCount = (std::size_t)N;
};

template <class T, glm::qualifier Q>
struct GlmTraits<glm::qua<T, Q>> {
    static constexpr bool kIsGlm = true;
    using Elem = T;
    static constexpr std::size_t kCount = 4;
};

// bg3se's Array is the engine's dynamically sized array. Its length and buffer
// members are private, so they are reached through size() and data(), which
// are public and constexpr -- instantiated per field type below, so no member
// offset is ever guessed at.
template <class T>
struct VectorTraits {
    static constexpr bool kIsVector = false;
    using Elem = void;
};

template <class T>
struct VectorTraits<Array<T>> {
    static constexpr bool kIsVector = true;
    using Elem = T;
};

template <class A>
std::size_t array_count_thunk(void const* container) {
    return (std::size_t)static_cast<A const*>(container)->size();
}

template <class A>
void* array_data_thunk(void const* container) {
    // const is dropped deliberately: the same descriptor serves reads and
    // writes, and a write has a non-const component to begin with.
    return (void*)static_cast<A const*>(container)->data();
}

// A hash set keeps its elements in a contiguous key array, which keys() hands
// back, so it reads as an array with no extra machinery. Writing one would
// desynchronise the table's hashes from its keys, so these are marked
// read-only rather than left writable.
template <class T>
struct SetTraits {
    static constexpr bool kIsSet = false;
    using Elem = void;
};

template <class T>
struct SetTraits<HashSet<T>> {
    static constexpr bool kIsSet = true;
    using Elem = T;
};

template <class S>
std::size_t set_count_thunk(void const* container) {
    return (std::size_t)static_cast<S const*>(container)->keys().size();
}

template <class S>
void* set_data_thunk(void const* container) {
    return (void*)static_cast<S const*>(container)->keys().data();
}

// std::optional is a value that may or may not be there, which is a different
// claim from one bg3le cannot read -- and reporting it as unsupported conflated
// the two. bg3se prints null for an empty one; saying "unsupported" instead
// would still be wrong when it is full.
//
// Read through has_value() and operator* rather than by guessing at where the
// engaged flag sits. The engine is built against libc++ and so is bg3le, which
// is already a requirement for std::string, so the layouts agree -- but going
// through the accessors means not depending on that here.
template <class T>
struct OptionalTraits {
    static constexpr bool kIsOptional = false;
    using Elem = void;
};

template <class T>
struct OptionalTraits<std::optional<T>> {
    static constexpr bool kIsOptional = true;
    using Elem = T;
};

template <class O>
std::size_t optional_count_thunk(void const* opt) {
    return static_cast<O const*>(opt)->has_value() ? 1 : 0;
}

template <class O>
void* optional_data_thunk(void const* opt) {
    auto const* o = static_cast<O const*>(opt);
    if (!o->has_value()) return nullptr;
    return (void*)&**o;
}

// A hash map keeps its keys and its values in two parallel contiguous runs,
// so slot i holds key i alongside value i -- which is what makes it
// presentable without hashing anything: iteration is a walk over both runs.
template <class T>
struct MapTraits {
    static constexpr bool kIsMap = false;
    using Key = void;
    using Value = void;
};

template <class K, class V>
struct MapTraits<HashMap<K, V>> {
    static constexpr bool kIsMap = true;
    using Key = K;
    using Value = V;
};

template <class M>
std::size_t map_count_thunk(void const* container) {
    return (std::size_t)static_cast<M const*>(container)->size();
}

template <class M>
void* map_values_thunk(void const* container) {
    return (void*)static_cast<M const*>(container)->raw_values().data();
}

template <class M>
void* map_keys_thunk(void const* container) {
    return (void*)static_cast<M const*>(container)->keys().data();
}

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
    // A FixedString is a four-byte index, so it behaves as a scalar here even
    // though resolving it needs the engine's string table.
    else if constexpr (std::is_same_v<T, FixedString>) return FieldKind::FixedString;
    else return FieldKind::Unsupported;
}

// An array of scalars is reported as one. A class type that is not a scalar in
// disguise is reported as a struct, and whether it can actually be traversed
// is decided at load by whether its type name resolves to a field table --
// so HashMap and DynamicArray land here too and simply fail to resolve, which
// is the honest answer until they are handled.
template <class T>
constexpr FieldKind kind_of() {
    // A fixed-extent array is one whatever its elements are: an element that
    // is a struct or another container is reached through the element
    // descriptor, and reportable_kind is what decides whether anything can be
    // done with it. Restricting this to scalar elements made
    // std::array<SomeStruct, N> unreadable -- which is how DiceValues, an
    // optional std::array of structs, stayed out of reach after the optional
    // itself worked.
    if constexpr (ArrayTraits<T>::kIsArray) {
        return FieldKind::ScalarArray;
    } else if constexpr (GlmTraits<T>::kIsGlm) {
        if constexpr (scalar_kind_of<typename GlmTraits<T>::Elem>()
                      != FieldKind::Unsupported) {
            return FieldKind::ScalarArray;
        } else {
            return FieldKind::Unsupported;
        }
    } else if constexpr (VectorTraits<T>::kIsVector
                         || SetTraits<T>::kIsSet) {
        return FieldKind::DynArray;
    } else if constexpr (MapTraits<T>::kIsMap) {
        return FieldKind::Map;
    } else if constexpr (OptionalTraits<T>::kIsOptional) {
        return FieldKind::Optional;
    } else if constexpr (scalar_kind_of<T>() != FieldKind::Unsupported) {
        return scalar_kind_of<T>();
    } else if constexpr (std::is_class_v<T>) {
        return FieldKind::Struct;
    } else {
        return FieldKind::Unsupported;
    }
}

// Not a field: records that a class also has the fields of another, named so
// it can be resolved at load rather than needing that class to be complete
// here.
constexpr FieldDesc inherit_field(char const* baseName) {
    FieldDesc f{};
    f.Name = baseName;
    f.Kind = FieldKind::Inherit;
    f.ElemKind = FieldKind::Unsupported;
    return f;
}

template <class T>
constexpr FieldDesc make_field(char const* name, std::size_t offset);

// A descriptor for a container's element type, so indexing a container can
// continue with the element's own accessors rather than with the field's.
template <class T>
inline constexpr FieldDesc kElementDesc = make_field<T>("(element)", 0);

// Builds a field descriptor from its type. Having every kind decision here
// rather than spelled out in each macro means adding a kind is one edit.
template <class T>
constexpr FieldDesc make_field(char const* name, std::size_t offset) {
    FieldDesc f{};
    f.Name = name;
    f.Offset = (std::uint32_t)offset;
    f.Size = (std::uint16_t)sizeof(T);
    f.Kind = kind_of<T>();
    f.ElemKind = FieldKind::Unsupported;
    f.ElemCount = 0;
    f.ElemSize = 0;
    f.TypeName = nullptr;
    f.TypeNameLength = 0;
    f.ElemTypeName = nullptr;
    f.ElemTypeNameLength = 0;

    // Element description, shared by both array kinds. A struct element is
    // named so it can be descended into, exactly as a struct field is.
    auto describe_elements = [&f]<class E>() {
        f.ElemKind = scalar_kind_of<E>();
        f.ElemSize = (std::uint16_t)sizeof(E);
        f.ElemDesc = &kElementDesc<E>;
        if constexpr (std::is_class_v<E>) {
            f.ElemTypeName = type_name<E>().data();
            f.ElemTypeNameLength = (std::uint16_t)type_name<E>().size();
        }
    };

    if constexpr (ArrayTraits<T>::kIsArray) {
        using E = typename ArrayTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.ElemCount = (std::uint16_t)ArrayTraits<T>::kCount;
    } else if constexpr (GlmTraits<T>::kIsGlm) {
        using E = typename GlmTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.ElemCount = (std::uint16_t)GlmTraits<T>::kCount;
    } else if constexpr (VectorTraits<T>::kIsVector) {
        using E = typename VectorTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &array_count_thunk<T>;
        f.Data = &array_data_thunk<T>;
    } else if constexpr (SetTraits<T>::kIsSet) {
        using E = typename SetTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &set_count_thunk<T>;
        f.Data = &set_data_thunk<T>;
        f.ReadOnly = true;
    } else if constexpr (OptionalTraits<T>::kIsOptional) {
        using E = typename OptionalTraits<T>::Elem;
        describe_elements.template operator()<E>();
        f.Count = &optional_count_thunk<T>;
        f.Data = &optional_data_thunk<T>;
    } else if constexpr (MapTraits<T>::kIsMap) {
        using K = typename MapTraits<T>::Key;
        using V = typename MapTraits<T>::Value;
        // The values are the elements; the keys are described separately.
        describe_elements.template operator()<V>();
        f.Count = &map_count_thunk<T>;
        f.Data = &map_values_thunk<T>;
        f.KeyData = &map_keys_thunk<T>;
        f.KeyKind = scalar_kind_of<K>();
        f.KeySize = (std::uint16_t)sizeof(K);
    } else if constexpr (std::is_class_v<T>) {
        f.TypeName = type_name<T>().data();
        f.TypeNameLength = (std::uint16_t)type_name<T>().size();
    } else if constexpr (std::is_enum_v<T>) {
        // An enum keeps its scalar kind, so it still reads and writes as an
        // integer; the type name is what lets the labels be found.
        f.TypeName = type_name<T>().data();
        f.TypeNameLength = (std::uint16_t)type_name<T>().size();
    }

    return f;
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

// Whether the entity's page holds a pointer to the component rather than the
// component itself.
//
// These exist and they are not rare: 46 of the 586 components a live save
// carries are proxies, including BoundComponent, esv::Character, esv::Item and
// every esv trigger. Reading one as though it were inline reads the pointer's
// own bytes as the first fields, which is not a subtle kind of wrong -- and it
// was only caught by comparing every component's declared size against the
// size the engine recorded, because the engine records 8 for all of them.
template <class T>
constexpr bool is_proxy_component() {
    if constexpr (IsProxyComponentType<T>) return true;
    else return false;
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
    // The size of the struct itself.
    std::size_t Size;
    // Whether the page holds a pointer to the component rather than the
    // component inline. For one of these the stride is a pointer and the
    // pointer has to be followed; see bg3le_meta_component_stride.
    bool IsProxy;
};

template <class T>
struct FieldTable;

// An enum's labels, so a field holding one reads as a name rather than as a
// number. bg3se generates these the same macro-driven way it generates the
// property maps, so they come out the same way: by including the generated
// file with different macros.
//
// A bitmask is kept apart from a plain enum because they present differently
// -- bg3se renders a bitmask as the list of set flags, and matching that
// matters for scripts written against it.
struct EnumLabel {
    char const* Name;
    std::uint64_t Value;
};

struct EnumDesc {
    std::string_view TypeName;
    char const* Name;
    bool IsBitmask;
    EnumLabel const* Labels;  // null-terminated
};

template <class T>
struct EnumTable;

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
            FieldDesc{},  /* the null terminator */                          \
        };                                                                    \
    };                                                                        \
    }

// Recorded as an entry rather than as a member, so it needs no surgery on the
// initialiser list and so multiple bases work. Resolved by name at load,
// because a class's table may be declared before its base's.
#define INHERIT(base)                                                         \
        inherit_field(#base),

#define PN(name, prop)                                                        \
        make_field<decltype(ObjectType::prop)>(                               \
            #name, offsetof(ObjectType, prop)),

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

// ---------------------------------------------------------------------------
// The enum labels, from the same generated metadata.
//
// Two passes, as with the property maps: one to declare a table per enum, one
// to collect them. The type is in scope inside the expansion, so both the
// table and the field that refers to it are named by the same type_name<T>()
// and compare exactly.
// ---------------------------------------------------------------------------

#define BEGIN_ENUM_IMPL(cls, bitmask)                                         \
    namespace bg3le {                                                         \
    template <> struct EnumTable<cls> {                                       \
        static constexpr std::string_view kTypeName = type_name<cls>();       \
        static constexpr char const* kName = #cls;                            \
        static constexpr bool kIsBitmask = bitmask;                           \
        static constexpr EnumLabel kLabels[] = {

#define BEGIN_ENUM(T, type, id) BEGIN_ENUM_IMPL(T, false)
#define BEGIN_BITMASK(T, type, id) BEGIN_ENUM_IMPL(T, true)
#define BEGIN_ENUM_NS(NS, T, luaName, type, id) BEGIN_ENUM_IMPL(NS::T, false)
#define BEGIN_BITMASK_NS(NS, T, luaName, type, id) BEGIN_ENUM_IMPL(NS::T, true)

#define EV(label, value) { #label, (std::uint64_t)(value) },

// Null-terminated, so a table with no values is still a valid array.
#define END_ENUM()                                                            \
            { nullptr, 0 },                                                   \
        };                                                                    \
    };                                                                        \
    }
#define END_ENUM_NS() END_ENUM()

#include <GameDefinitions/Generated/Enumerations.inl>
#include <GameDefinitions/Generated/ExternalEnumerations.inl>

#undef BEGIN_ENUM_IMPL
#undef BEGIN_ENUM
#undef BEGIN_BITMASK
#undef BEGIN_ENUM_NS
#undef BEGIN_BITMASK_NS
#undef EV
#undef END_ENUM
#undef END_ENUM_NS

namespace bg3le {
namespace {

template <class T>
inline constexpr EnumDesc kEnumDesc{
    EnumTable<T>::kTypeName,
    EnumTable<T>::kName,
    EnumTable<T>::kIsBitmask,
    EnumTable<T>::kLabels,
};

constexpr EnumDesc const* kAllEnums[] = {
#define BEGIN_ENUM(T, type, id) &kEnumDesc<T>,
#define BEGIN_BITMASK(T, type, id) &kEnumDesc<T>,
#define BEGIN_ENUM_NS(NS, T, luaName, type, id) &kEnumDesc<NS::T>,
#define BEGIN_BITMASK_NS(NS, T, luaName, type, id) &kEnumDesc<NS::T>,
#define EV(label, value)
#define END_ENUM()
#define END_ENUM_NS()
#include <GameDefinitions/Generated/Enumerations.inl>
#include <GameDefinitions/Generated/ExternalEnumerations.inl>
#undef BEGIN_ENUM
#undef BEGIN_BITMASK
#undef BEGIN_ENUM_NS
#undef BEGIN_BITMASK_NS
#undef EV
#undef END_ENUM
#undef END_ENUM_NS
};

// Enums by their type_name<T>() spelling, the same index the class tables use.
std::unordered_map<std::string_view, EnumDesc const*>& by_enum_name() {
    static std::unordered_map<std::string_view, EnumDesc const*> map = [] {
        std::unordered_map<std::string_view, EnumDesc const*> m;
        m.reserve(std::size(kAllEnums));
        for (auto const* e : kAllEnums) m.emplace(e->TypeName, e);
        return m;
    }();
    return map;
}

}  // namespace
}  // namespace bg3le

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
    is_proxy_component<T>(),
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

// The table describing an array field's element type, or null.
ClassFields const* elem_type_of(FieldDesc const* field) {
    if (field == nullptr || field->ElemTypeName == nullptr) return nullptr;
    auto it = by_type_name().find(
        std::string_view(field->ElemTypeName, field->ElemTypeNameLength));
    return it != by_type_name().end() ? it->second : nullptr;
}

// What a path resolved to.
//
// Address is only filled in when a base was supplied. It has to be, rather
// than an offset being enough, because crossing a dynamic array means
// following its buffer pointer -- the element does not live at a fixed offset
// from the component at all.
struct Resolved {
    FieldDesc Field{};      // synthesised for an element, copied for a field
    void* Address{nullptr};
    bool Ok{false};
};

// Resolves a path, which may name nested fields and index arrays:
//
//   "Hp"                     a field
//   "Transform.Translate"    a field of a nested struct
//   "Resources[2].Amount"    a field of an element of a dynamic array
//
// base may be null to resolve the type only, which is what listing fields and
// reporting kinds need; then Address stays null and an array index is still
// crossed, because the element type is known statically even when the element
// address is not.
Resolved resolve_path(ClassFields const* cls, char const* path, void* base) {
    Resolved out;
    if (cls == nullptr || path == nullptr) return out;

    std::string_view rest(path);
    void* address = base;

    // Bounded so a pathological path cannot spin; nothing nests near this
    // deep.
    for (unsigned step = 0; step < 16; ++step) {
        // A segment is a name, optionally followed by [index], and the path
        // continues after a dot.
        const auto dot = rest.find('.');
        std::string_view segment =
            dot == std::string_view::npos ? rest : rest.substr(0, dot);
        if (segment.empty()) return out;

        // A segment may carry more than one subscript, because indexing a
        // container can yield another one: a map of arrays reads as
        // "Resources[0][1]".
        std::string_view subscripts;
        if (const auto open = segment.find('['); open != std::string_view::npos) {
            if (segment.back() != ']') return out;
            subscripts = segment.substr(open);
            segment = segment.substr(0, open);
            if (segment.empty()) return out;
        }

        // find_field takes a NUL-terminated name; a path segment is not one.
        const std::string name(segment);
        FieldDesc const* field = find_field(cls, name.c_str());
        if (field == nullptr) return out;

        FieldDesc current = *field;
        if (address != nullptr) {
            address = (char*)address + field->Offset;
        }

        // Apply each subscript in turn, the element descriptor of one becoming
        // the container of the next.
        while (!subscripts.empty()) {
            if (subscripts.front() != '[') return out;
            const auto close = subscripts.find(']');
            if (close == std::string_view::npos) return out;

            const auto digits = subscripts.substr(1, close - 1);
            if (digits.empty()) return out;
            std::size_t index = 0;
            for (const char c : digits) {
                if (c < '0' || c > '9') return out;
                index = index * 10 + (std::size_t)(c - '0');
                if (index > 0xffffff) return out;  // absurd; refuse
            }
            subscripts = subscripts.substr(close + 1);

            if (current.ElemSize == 0 || current.ElemDesc == nullptr) return out;

            switch (current.Kind) {
            case FieldKind::ScalarArray:
                if (index >= current.ElemCount) return out;
                if (address != nullptr) {
                    address = (char*)address + index * current.ElemSize;
                }
                break;

            // A map indexes to its value, which is what makes the value side
            // reachable by slot; the key side is read separately, because a
            // path has no way to say "the key of this slot".
            case FieldKind::DynArray:
            case FieldKind::Map:
            case FieldKind::Optional: {
                if (current.Count == nullptr || current.Data == nullptr) return out;
                if (address != nullptr) {
                    if (index >= current.Count(address)) return out;
                    void* data = current.Data(address);
                    if (data == nullptr) return out;
                    address = (char*)data + index * current.ElemSize;
                }
                break;
            }

            default:
                return out;  // not a container; nothing to index
            }

            // Continue with the element's own descriptor, which carries its
            // accessors if it is itself a container. Read-only propagates: an
            // element of a hash set is one of the keys its hashes were
            // computed from.
            const bool readOnly = current.ReadOnly;
            char const* fieldName = current.Name;
            current = *current.ElemDesc;
            current.Name = fieldName;
            current.ReadOnly = current.ReadOnly || readOnly;
        }

        if (dot == std::string_view::npos) {
            out.Field = current;
            out.Address = address;
            out.Ok = true;
            return out;
        }

        cls = struct_type_of(&current);
        if (cls == nullptr) return out;  // cannot descend through this
        rest = rest.substr(dot + 1);
    }
    return out;
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

// The stride the engine walks the component page with, which is what
// GetComponent multiplies the entity's slot by. For a proxy component that is
// a pointer, not the struct: the struct lives wherever the pointer says.
extern "C" std::size_t bg3le_meta_component_stride(void const* handle) {
    if (handle == nullptr) return 0;
    auto const* cls = static_cast<ClassFields const*>(handle);
    return cls->IsProxy ? sizeof(void*) : cls->Size;
}

extern "C" bool bg3le_meta_component_is_proxy(void const* handle) {
    if (handle == nullptr) return false;
    return static_cast<ClassFields const*>(handle)->IsProxy;
}

namespace {

// The kind to report for a resolved field. A struct with no table behind it,
// and an array whose elements are structs with no table, cannot be acted on,
// so they are reported as unsupported rather than as something traversable.
std::uint8_t reportable_kind(FieldDesc const& field, unsigned depth = 0) {
    if (depth > 4) return (std::uint8_t)FieldKind::Unsupported;

    if (field.Kind == FieldKind::Struct && struct_type_of(&field) == nullptr) {
        return (std::uint8_t)FieldKind::Unsupported;
    }

    if (field.Kind == FieldKind::ScalarArray || field.Kind == FieldKind::DynArray
        || field.Kind == FieldKind::Map || field.Kind == FieldKind::Optional) {
        // A container is usable if its elements are: a scalar, a struct bg3se
        // describes, or another container. That last case is why this
        // recurses rather than testing the element fields directly -- a
        // HashMap<Guid, Array<Entry>> has no scalar element kind and no
        // element struct, but indexing it twice reaches an Entry, so reporting
        // it unsupported would hide a field that works.
        if (field.ElemKind != FieldKind::Unsupported) {
            return (std::uint8_t)field.Kind;
        }
        if (elem_type_of(&field) != nullptr) {
            return (std::uint8_t)field.Kind;
        }
        if (field.ElemDesc != nullptr
            && reportable_kind(*field.ElemDesc, depth + 1)
                   != (std::uint8_t)FieldKind::Unsupported) {
            return (std::uint8_t)field.Kind;
        }
        return (std::uint8_t)FieldKind::Unsupported;
    }

    return (std::uint8_t)field.Kind;
}

}  // namespace

// Resolves a field of a component by name or by dotted path, following base
// classes. Type information only -- no component instance, so a path may
// index an array (the element type is static) but the offset returned is not
// meaningful once it has, because a dynamic array's elements do not live at a
// fixed offset from the component. Use bg3le_meta_resolve to reach a value.
extern "C" bool bg3le_meta_field(void const* handle, char const* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount) {
    if (handle == nullptr || name == nullptr) return false;
    const auto r =
        resolve_path(static_cast<ClassFields const*>(handle), name, nullptr);
    if (!r.Ok) return false;
    *offset = r.Field.Offset;
    *size = r.Field.Size;
    *kind = reportable_kind(r.Field);
    *elemKind = (std::uint8_t)r.Field.ElemKind;
    *elemCount = r.Field.ElemCount;
    return true;
}

// Resolves a path against a live component and hands back the address of the
// value, following array buffers where the path indexes one.
extern "C" bool bg3le_meta_resolve(void const* handle, char const* path,
                                   void* component, void** address,
                                   std::uint8_t* kind, std::uint16_t* size,
                                   bool* readOnly) {
    *address = nullptr;
    *readOnly = false;
    if (handle == nullptr || path == nullptr || component == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;

    *address = r.Address;
    *kind = reportable_kind(r.Field);
    *size = r.Field.Size;
    *readOnly = r.Field.ReadOnly;
    return true;
}

// The current length of a dynamic array, and the element stride. Needs the
// component because the length is stored in the container.
// Status rather than a bool, because the caller has to be able to tell "this
// resolved and holds nothing" from "this did not resolve". Collapsing the two
// is how an unreadable container came to look like an empty one.
//
// 0 ok, 1 bad arguments, 2 the path does not resolve, 3 not a container,
// 4 a container with no length accessor.
extern "C" int bg3le_meta_array_length(void const* handle, char const* path,
                                       void* component, std::size_t* count,
                                       std::uint16_t* elemSize,
                                       std::uint8_t* elemKind) {
    *count = 0;
    if (handle == nullptr || path == nullptr || component == nullptr) return 1;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return 2;

    *elemSize = r.Field.ElemSize;
    *elemKind = (std::uint8_t)r.Field.ElemKind;

    if (r.Field.Kind == FieldKind::ScalarArray) {
        *count = r.Field.ElemCount;
        return 0;
    }
    if (r.Field.Kind != FieldKind::DynArray && r.Field.Kind != FieldKind::Map
        && r.Field.Kind != FieldKind::Optional) {
        return 3;
    }
    if (r.Field.Count == nullptr) return 4;

    *count = r.Field.Count(r.Address);
    return 0;
}

// The key of one slot of a map.
//
// Keys are read by slot rather than looked up, because a lookup would mean
// hashing a key built from Lua -- for every key type, with the engine's own
// hash. Slot i holds the key belonging to the value at the same index, so
// walking the slots pairs them up, and these maps are small enough for a
// caller to walk.
extern "C" bool bg3le_meta_map_key(void const* handle, char const* path,
                                   void* component, std::size_t index,
                                   void** address, std::uint8_t* kind,
                                   std::uint16_t* size) {
    *address = nullptr;
    if (handle == nullptr || path == nullptr || component == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                component);
    if (!r.Ok || r.Address == nullptr) return false;
    if (r.Field.Kind != FieldKind::Map) return false;
    if (r.Field.Count == nullptr || r.Field.KeyData == nullptr) return false;
    if (r.Field.KeySize == 0) return false;
    if (index >= r.Field.Count(r.Address)) return false;

    void* keys = r.Field.KeyData(r.Address);
    if (keys == nullptr) return false;

    *address = (char*)keys + index * r.Field.KeySize;
    *kind = (std::uint8_t)r.Field.KeyKind;
    *size = r.Field.KeySize;
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
        const auto r = resolve_path(cls, path, nullptr);
        if (!r.Ok) return 0;
        // Either a nested struct, or an element of an array of structs; both
        // resolve to a field whose type has a table.
        cls = struct_type_of(&r.Field);
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
            kinds[n] = reportable_kind(*f);
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
// Walks a real component holding a real dynamic array, so the container
// accessors and the indexed path walk are exercised on genuine memory rather
// than inferred from the tables.
//
// Everything the static tables say can be checked from outside (see
// tools/meta-check.c), but following a dynamic array's buffer cannot: it needs
// an instance. Building one here is the only way to test that without the
// game.
//
// Pushing onto the array allocates through bg3se, so the caller has to install
// an allocator first. In the test harness that is malloc, which is safe
// precisely because this memory is never handed to the engine.
//
// Returns the number of failed checks, and writes a line per failure.
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity);
extern "C" bool bg3le_meta_enum_label(void const* handle, char const* path,
                                      std::size_t index, char const** label,
                                      std::uint64_t* value, bool* isBitmask);

extern "C" int bg3le_meta_selftest() {
    int failures = 0;
    auto fail = [&failures](char const* what) {
        bg3le::logf("meta selftest: FAIL %s", what);
        ++failures;
    };

    if (!bg3le_game_allocator_ready()) {
        fail("no allocator installed; cannot build a test array");
        return failures;
    }

    ActionResourceEventsOneFrameComponent component;
    ActionResourceSetValueRequest a{};
    a.Amount = 11.5;
    a.OldAmount = 1.0;
    ActionResourceSetValueRequest b{};
    b.Amount = 22.25;
    b.OldAmount = 2.0;
    component.Events.push_back(a);
    component.Events.push_back(b);

    auto const* meta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::ActionResourceEventsOneFrameComponent"));
    if (meta == nullptr) {
        fail("no metadata for ActionResourceEventsOneFrameComponent");
        return failures;
    }

    // The length has to come from the container, not from the table.
    std::size_t count = 0;
    std::uint16_t elemSize = 0;
    std::uint8_t elemKind = 0;
    if (bg3le_meta_array_length(meta, "Events", &component, &count, &elemSize,
                                &elemKind) != 0) {
        fail("Events has no array length");
    } else {
        if (count != 2) fail("Events length is not 2");
        if (elemSize != sizeof(ActionResourceSetValueRequest)) {
            fail("Events element stride does not match the element type");
        }
    }

    // Indexing the array and then descending into the element struct.
    void* address = nullptr;
    std::uint8_t kind = 0;
    std::uint16_t size = 0;
    bool readOnly = false;
    if (!bg3le_meta_resolve(meta, "Events[1].Amount", &component, &address,
                            &kind, &size, &readOnly)) {
        fail("Events[1].Amount does not resolve");
    } else if (address != &component.Events[1].Amount) {
        fail("Events[1].Amount resolved to the wrong address");
    } else if (kind != (std::uint8_t)FieldKind::Double) {
        fail("Events[1].Amount is not reported as a double");
    } else if (*(double*)address != 22.25) {
        fail("Events[1].Amount does not read back what was written");
    }

    if (bg3le_meta_resolve(meta, "Events[0].OldAmount", &component, &address,
                           &kind, &size, &readOnly)
        && address != &component.Events[0].OldAmount) {
        fail("Events[0].OldAmount resolved to the wrong address");
    }

    // Past the end has to fail rather than run off the buffer.
    if (bg3le_meta_resolve(meta, "Events[2].Amount", &component, &address,
                           &kind, &size, &readOnly)) {
        fail("Events[2] resolved despite the array holding two elements");
    }

    // Descending into a dynamic array without indexing it is not meaningful
    // and must fail rather than read the container's own bytes as a struct.
    if (bg3le_meta_resolve(meta, "Events.Amount", &component, &address, &kind,
                           &size, &readOnly)) {
        fail("Events.Amount resolved without an index");
    }

    // A write through a resolved address has to land in the component.
    if (bg3le_meta_resolve(meta, "Events[0].Amount", &component, &address,
                           &kind, &size, &readOnly)) {
        *(double*)address = 99.5;
        if (component.Events[0].Amount != 99.5) {
            fail("a write through a resolved address did not land");
        }
    } else {
        fail("Events[0].Amount does not resolve");
    }

    // A hash set reads as an array of its keys, and has to report itself
    // read-only: its elements are the keys the table's hashes were computed
    // from, so writing one in place would desynchronise the two.
    SummonContainerComponent summons;
    summons.Characters.insert(EntityHandle((std::uint64_t)0x1234));
    summons.Characters.insert(EntityHandle((std::uint64_t)0x5678));

    auto const* summonMeta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::summon::ContainerComponent"));
    if (summonMeta == nullptr) {
        fail("no metadata for the summon container component");
    } else {
        std::size_t setCount = 0;
        std::uint16_t setElemSize = 0;
        std::uint8_t setElemKind = 0;
        if (bg3le_meta_array_length(summonMeta, "Characters", &summons,
                                    &setCount, &setElemSize, &setElemKind)
            != 0) {
            fail("Characters has no array length");
        } else if (setCount != 2) {
            fail("Characters length is not 2");
        }

        if (!bg3le_meta_resolve(summonMeta, "Characters[0]", &summons, &address,
                                &kind, &size, &readOnly)) {
            fail("Characters[0] does not resolve");
        } else {
            if (!readOnly) fail("a hash set element is not reported read-only");
            if (address != summons.Characters.keys().data()) {
                fail("Characters[0] is not the first key");
            }
        }
    }

    // A map, which is the deepest walk: a HashMap<Guid, Array<...>> means
    // indexing the map to a value that is itself an array, indexing that, and
    // then descending into the element struct. That is the whole chain
    // "Resources[0][1].Amount" exercised on real memory, and it is also the
    // shape LenonTweaks needs.
    ActionResourcesComponent resources;
    const auto key = Guid{0x1122334455667788ull, 0x99aabbccddeeff00ull};
    Array<ActionResourceEntry> entries;
    ActionResourceEntry e0{};
    e0.Amount = 3.5;
    e0.MaxAmount = 4.0;
    ActionResourceEntry e1{};
    e1.Amount = 7.25;
    e1.MaxAmount = 8.0;
    entries.push_back(e0);
    entries.push_back(e1);
    resources.Resources.set(key, entries);

    auto const* resMeta = static_cast<ClassFields const*>(
        bg3le_meta_component("eoc::ActionResourcesComponent"));
    if (resMeta == nullptr) {
        fail("no metadata for the action resources component");
    } else {
        std::size_t mapCount = 0;
        std::uint16_t mapElemSize = 0;
        std::uint8_t mapElemKind = 0;
        if (bg3le_meta_array_length(resMeta, "Resources", &resources,
                                    &mapCount, &mapElemSize, &mapElemKind)
            != 0) {
            fail("Resources has no length");
        } else if (mapCount != 1) {
            fail("Resources length is not 1");
        }

        // The key run, read by slot.
        if (!bg3le_meta_map_key(resMeta, "Resources", &resources, 0, &address,
                                &kind, &size)) {
            fail("Resources has no key at slot 0");
        } else {
            if (kind != (std::uint8_t)FieldKind::Guid) {
                fail("the Resources key is not reported as a Guid");
            }
            if (*(Guid*)address != key) fail("the Resources key does not match");
        }

        // A key past the end has to fail rather than run off the run.
        if (bg3le_meta_map_key(resMeta, "Resources", &resources, 1, &address,
                               &kind, &size)) {
            fail("Resources returned a key at slot 1 of a one-entry map");
        }

        // Map -> value array -> element -> field, in one path.
        if (!bg3le_meta_resolve(resMeta, "Resources[0][1].Amount", &resources,
                                &address, &kind, &size, &readOnly)) {
            fail("Resources[0][1].Amount does not resolve");
        } else if (address != &resources.Resources.values()[0][1].Amount) {
            fail("Resources[0][1].Amount resolved to the wrong address");
        } else if (*(double*)address != 7.25) {
            fail("Resources[0][1].Amount does not read back what was written");
        }

        if (!bg3le_meta_resolve(resMeta, "Resources[0][0].MaxAmount",
                                &resources, &address, &kind, &size,
                                &readOnly)) {
            fail("Resources[0][0].MaxAmount does not resolve");
        } else if (*(double*)address != 4.0) {
            fail("Resources[0][0].MaxAmount does not read back");
        }

        // Indexing past the end of the inner array, and past the map.
        if (bg3le_meta_resolve(resMeta, "Resources[0][2].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            fail("the inner array indexed past its two entries");
        }
        if (bg3le_meta_resolve(resMeta, "Resources[1][0].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            fail("the map indexed past its one entry");
        }

        // The write the mod actually wants: top an entry up to its maximum.
        if (bg3le_meta_resolve(resMeta, "Resources[0][0].Amount", &resources,
                               &address, &kind, &size, &readOnly)) {
            *(double*)address = 4.0;
            if (resources.Resources.values()[0][0].Amount != 4.0) {
                fail("a write into a map's array element did not land");
            }
        } else {
            fail("Resources[0][0].Amount does not resolve");
        }
    }

    // Guid formatting, pinned two ways.
    //
    // Against a literal, because the byte order is not guessable: the last
    // eight bytes are pairwise swapped as well as the first three groups, and
    // getting that wrong produced a UUID that looked entirely plausible.
    //
    // And as a round trip, because UuidToHandle parses what this formats -- a
    // UUID read out of a component has to be handed straight back.
    {
        const auto known = Guid{0x1122334455667788ull, 0x99aabbccddeeff00ull};
        char text[40];
        if (!bg3le_meta_format_guid(&known, text, sizeof(text))) {
            fail("formatting a Guid did not fit its buffer");
        } else if (std::strcmp(text, "55667788-3344-1122-ff00-ddeebbcc99aa")
                   != 0) {
            bg3le::logf("meta selftest: FAIL Guid formatted as %s", text);
            ++failures;
        }

        const auto parsed = Guid::ParseGuidString(text);
        if (!parsed.has_value()) {
            fail("a formatted Guid does not parse back");
        } else if (*parsed != known) {
            fail("a Guid does not survive a format and parse round trip");
        }

        // Too small a buffer has to be refused rather than truncated, since a
        // truncated UUID would still look like one.
        char small[8];
        if (bg3le_meta_format_guid(&known, small, sizeof(small))) {
            fail("formatting a Guid into a short buffer was allowed");
        }
    }

    // An optional, empty and then full. Empty has to be distinguishable from
    // unreadable: bg3se prints null for an empty one, and reporting it as
    // unsupported conflated "there is nothing here" with "I cannot read this".
    {
        auto const* resMeta3 = static_cast<ClassFields const*>(
            bg3le_meta_component("eoc::ActionResourcesComponent"));
        std::size_t held = 0;
        std::uint16_t sz = 0;
        std::uint8_t ek = 0;

        // The entries above were default-constructed, so DiceValues is empty.
        if (bg3le_meta_array_length(resMeta3, "Resources[0][0].DiceValues",
                                    &resources, &held, &sz, &ek) != 0) {
            fail("an empty optional has no length");
        } else if (held != 0) {
            fail("an empty optional does not report as empty");
        }

        // Fill it; the same field has to report one and read back.
        std::array<ActionResourceDiceValue, 7> dice{};
        dice[0].Amount = 3.0;
        dice[0].MaxAmount = 6.0;
        resources.Resources.values()[0][0].DiceValues = dice;

        if (bg3le_meta_array_length(resMeta3, "Resources[0][0].DiceValues",
                                    &resources, &held, &sz, &ek) != 0) {
            fail("a full optional has no length");
        } else if (held != 1) {
            fail("a full optional does not report as holding one");
        }

        if (!bg3le_meta_resolve(resMeta3,
                                "Resources[0][0].DiceValues[0][0].Amount",
                                &resources, &address, &kind, &size,
                                &readOnly)) {
            fail("cannot reach through a full optional");
        } else if (*(double*)address != 3.0) {
            fail("a value read through an optional does not match");
        }

        // Past the single slot must fail, as for any container.
        if (bg3le_meta_resolve(resMeta3, "Resources[0][0].DiceValues[1]",
                               &resources, &address, &kind, &size,
                               &readOnly)) {
            fail("an optional indexed past its single slot resolved");
        }
    }

    // Enum labels, checked against what bg3se prints on Windows for the same
    // save: ReplenishType came back as 2 and 8 here where bg3se showed
    // ["Default"] and ["Rest"]. It is a bitmask, so the labels are flags.
    {
        auto const* resMeta2 = static_cast<ClassFields const*>(
            bg3le_meta_component("eoc::ActionResourcesComponent"));
        char const* label = nullptr;
        std::uint64_t value = 0;
        bool isBitmask = false;

        bool foundDefault = false;
        bool foundRest = false;
        for (std::size_t i = 0;
             bg3le_meta_enum_label(resMeta2, "Resources[0][0].ReplenishType", i,
                                   &label, &value, &isBitmask);
             ++i) {
            if (std::strcmp(label, "Default") == 0 && value == 0x02) {
                foundDefault = true;
            }
            if (std::strcmp(label, "Rest") == 0 && value == 0x08) {
                foundRest = true;
            }
        }
        if (!isBitmask) fail("ReplenishType is not reported as a bitmask");
        if (!foundDefault) fail("ReplenishType has no Default = 2");
        if (!foundRest) fail("ReplenishType has no Rest = 8");

        // A field that is not an enum must report none, rather than the
        // labels of whatever happens to share its integer kind.
        if (bg3le_meta_enum_label(meta, "Events[0].Amount", 0, &label, &value,
                                  &isBitmask)) {
            fail("a non-enum field reported enum labels");
        }
    }

    if (failures == 0) {
        bg3le::logf("meta selftest: the container walks behave");
    }
    return failures;
}

extern "C" std::size_t bg3le_meta_class_count() { return std::size(kAllClasses); }

// Formats a Guid the way the engine spells it.
//
// Not open-coded on the bg3le side, because the byte order is not the obvious
// one: the first three groups are little-endian words, as a Microsoft GUID is,
// but so are the last eight bytes, pairwise. Hand-rolling it produced
// 8411-0cc7dfacdcfc where the engine writes 1184-c70cacdffcdc -- a UUID that
// looked entirely plausible and was wrong. Going through bg3se's own ToString
// also keeps this the exact inverse of the ParseGuidString that UuidToHandle
// relies on, so a UUID read out of a component can be handed straight back.
//
// Writes at most capacity bytes including the terminator, and returns false if
// it would not fit.
extern "C" bool bg3le_meta_format_guid(void const* bytes, char* out,
                                       std::size_t capacity) {
    if (bytes == nullptr || out == nullptr || capacity == 0) return false;

    const auto text = static_cast<Guid const*>(bytes)->ToString();
    if (text.size() + 1 > capacity) return false;

    std::memcpy(out, text.c_str(), text.size() + 1);
    return true;
}

// The i'th class, for sweeping the whole set -- listing the components a
// script can reach, or measuring how much of them converts.
extern "C" void const* bg3le_meta_class_at(std::size_t index) {
    if (index >= std::size(kAllClasses)) return nullptr;
    return kAllClasses[index];
}

extern "C" std::size_t bg3le_meta_component_count() {
    std::size_t n = 0;
    for (auto const* cls : kAllClasses) {
        if (cls->EngineClass != nullptr) ++n;
    }
    return n;
}

// The labels of an enum-typed field.
//
// Reported separately from the field's kind, which stays the underlying
// integer, so an enum still reads and writes as a number for anything that
// wants one. index walks the labels; a bitmask is told apart because bg3se
// renders one as the list of set flags and matching that keeps scripts
// written against it working.
//
// Returns false once index runs past the end, or immediately if the field is
// not an enum.
extern "C" bool bg3le_meta_enum_label(void const* handle, char const* path,
                                      std::size_t index, char const** label,
                                      std::uint64_t* value, bool* isBitmask) {
    *label = nullptr;
    if (handle == nullptr || path == nullptr) return false;

    const auto r = resolve_path(static_cast<ClassFields const*>(handle), path,
                                nullptr);
    if (!r.Ok || r.Field.TypeName == nullptr) return false;

    auto it = by_enum_name().find(
        std::string_view(r.Field.TypeName, r.Field.TypeNameLength));
    if (it == by_enum_name().end()) return false;

    *isBitmask = it->second->IsBitmask;
    auto const* labels = it->second->Labels;
    for (std::size_t i = 0; i < index; ++i) {
        if (labels[i].Name == nullptr) return false;
    }
    if (labels[index].Name == nullptr) return false;

    *label = labels[index].Name;
    *value = labels[index].Value;
    return true;
}

// The name of a field kind.
//
// The single source of truth for these, because they were duplicated in
// lua_host.cpp and inserting a kind into the middle of the enum silently
// renumbered everything after it -- which broke the checks that asserted on
// numbers, and would have mislabelled every kind after the insertion had the
// two lists ever disagreed.
extern "C" char const* bg3le_meta_kind_name(std::uint8_t kind) {
    switch ((FieldKind)kind) {
        case FieldKind::Bool: return "boolean";
        case FieldKind::Float: return "float";
        case FieldKind::Double: return "double";
        case FieldKind::Int8: return "int8";
        case FieldKind::Uint8: return "uint8";
        case FieldKind::Int16: return "int16";
        case FieldKind::Uint16: return "uint16";
        case FieldKind::Int32: return "int32";
        case FieldKind::Uint32: return "uint32";
        case FieldKind::Int64: return "int64";
        case FieldKind::Uint64: return "uint64";
        case FieldKind::Guid: return "guid";
        case FieldKind::Entity: return "entity";
        case FieldKind::FixedString: return "string";
        case FieldKind::ScalarArray: return "array";
        case FieldKind::Struct: return "struct";
        case FieldKind::DynArray: return "array";
        case FieldKind::Map: return "map";
        case FieldKind::Optional: return "optional";
        case FieldKind::Inherit: return "inherit";
        default: return "unsupported";
    }
}

// How many enums carry labels, for the startup log.
extern "C" std::size_t bg3le_meta_enum_count() { return std::size(kAllEnums); }

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
