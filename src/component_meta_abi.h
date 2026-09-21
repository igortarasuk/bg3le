// The shape of the component field tables, shared between bg3le and the
// translation unit that extracts them from bg3se's generated metadata.
//
// Kept free of bg3se headers deliberately: src/vendor/component_meta.cpp is
// the only place that includes them, and everything else works from this.
#pragma once

#include <cstddef>
#include <cstdint>

namespace bg3le {

// What a field holds. Only the kinds bg3le can convert without interpretation
// are named; everything else is Unsupported and is reported as such rather
// than guessed at.
enum class FieldKind : std::uint8_t {
    Unsupported = 0,
    Bool,
    Float,
    Double,
    Int8,
    Uint8,
    Int16,
    Uint16,
    Int32,
    Uint32,
    Int64,
    Uint64,
    Guid,
    Entity,
    // A fixed-extent array of one of the scalar kinds above, which is how the
    // engine stores the per-ability and per-skill tables. ElemKind and
    // ElemCount describe the elements.
    ScalarArray,
    // A nested struct. TypeName names its type; whether it is traversable
    // depends on bg3se describing that type too, which is resolved by name at
    // load rather than at compile time -- a field's type does not have to have
    // a field table of its own for the field itself to be recorded.
    Struct,
    // Not a field: records that the class also has the fields of the class
    // named in Name. Classes are declared in dependency-free order, so bases
    // are resolved by name at load rather than by pointer.
    Inherit,
};

struct FieldDesc {
    char const* Name;       // a string literal from the generated metadata
    std::uint32_t Offset;
    std::uint16_t Size;
    FieldKind Kind;
    FieldKind ElemKind;       // ScalarArray only
    std::uint16_t ElemCount;  // ScalarArray only
    // The field's C++ type, for Struct. Not NUL-terminated: it is a slice of a
    // compiler-generated function-name string, so it carries its own length.
    char const* TypeName;
    std::uint16_t TypeNameLength;
};

}  // namespace bg3le
