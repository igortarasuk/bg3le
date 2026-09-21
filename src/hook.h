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

#include <cstddef>
#include <cstdint>

namespace bg3le {

// Replaces the pointer in slot_offset with replacement, first checking that
// it currently holds expected_offset. The check is what makes this safe
// across game patches: if the binary has shifted, the hook is refused rather
// than corrupting an unrelated table. Both offsets are link-time addresses;
// the load bias is applied internally.
bool hook_slot(std::uintptr_t slot_offset, std::uintptr_t expected_offset,
               void* replacement, void** original);

// Redirects every direct `call rel32` that targets func_offset to
// replacement, for functions that are called directly rather than through a
// pointer table. Each site is verified to be an E8 whose displacement
// actually resolves to the target before being touched.
//
// rel32 cannot reach our library from the executable's text, so the calls
// are pointed at a trampoline allocated within +/-2GB of the code. Returns
// the number of sites patched; original receives the real function address.
std::size_t hook_call_sites(std::uintptr_t func_offset, void* replacement,
                            void** original);

}  // namespace bg3le
