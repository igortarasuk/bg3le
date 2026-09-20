// Hooking by function-pointer table slot.
//
// Larian's functions have no symbols and no PLT entries, so neither dynamic
// interposition nor linker tricks reach them. But virtual and
// callback-dispatched functions have their address stored in a table via an
// R_X86_64_RELATIVE relocation, and replacing that pointer is a single
// aligned store: no prologue relocation, no instruction length decoder, no
// trampoline, and trivially reversible.
//
// Slot and function addresses come from tools/recover_symbols.py piped
// through tools/find_slots.py.
#pragma once

#include <cstdint>

namespace bg3le {

// Replaces the pointer in slot_offset with replacement, first checking that
// it currently holds expected_offset. The check is what makes this safe
// across game patches: if the binary has shifted, the hook is refused rather
// than corrupting an unrelated table. Both offsets are link-time addresses;
// the load bias is applied internally.
bool hook_slot(std::uintptr_t slot_offset, std::uintptr_t expected_offset,
               void* replacement, void** original);

}  // namespace bg3le
