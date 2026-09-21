// The engine's ECS type-index registry, read from the executable's symbol
// table.
//
// Every ECS type the engine registers gets a function-local static holding the
// index it was assigned at startup, and clang emits a symbol for it:
//
//   ls::TypeId<eoc::HealthComponent, ecs::ComponentTypeIdContext>::m_TypeIndex
//
// So the whole name -> index mapping is available by enumerating .symtab. The
// Windows extender has to recover the same information by scanning the image
// for byte patterns and reading displacement fields at hardcoded instruction
// offsets, because MSVC emits nothing nameable.
//
// The indices are assigned during engine startup, so the addresses are stable
// but the values must be read when they are needed, not cached at load.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "elf_symbols.h"

namespace bg3le {
namespace ecs {

// The registration contexts the engine uses. These correspond to the contexts
// bg3se recognises in EntitySystemHelpersBase::UpdateComponentMappings.
enum class Context {
    Component,          // ecs::ComponentTypeIdContext
    OneFrameComponent,  // ecs::OneFrameComponentTypeIdContext
    System,             // ecs::SystemsContext
    Replication,        // ecs::sync::ReplicatedTypeContext
    ImmutableData,      // ls::ImmutableDataHeadmaster
    Unchecked,          // ecs::UncheckedComponentTypeIdContext
    Other,
};

// Builds the mapping from the given symbol table. Safe to call more than once;
// later calls are ignored. Returns the number of types recorded.
std::size_t load(const SymbolTable& symbols);

// The index the engine assigned to a type, read live. Returns nothing if the
// type is not registered in that context, or if the index still reads as
// unassigned (the engine fills these in during startup).
std::optional<std::int32_t> index_of(Context context, const std::string& name);

// Number of types recorded in a context.
std::size_t count(Context context);

// For diagnostics: the name of a context.
const char* context_name(Context context);

}  // namespace ecs
}  // namespace bg3le
