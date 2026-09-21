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
};

}  // namespace bg3le
