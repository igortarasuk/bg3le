// The recording half of bg3le's Detours stand-in. See detour_interpose.cpp.
#pragma once

#include <cstddef>

namespace bg3le {

// The replacement bg3se registered against this function, or null.
void* detour_for(void const* original);

// Tells the shim that `forwarder` is bg3le's exported stand-in for `real`.
//
// This is what keeps a hook from calling itself. bg3se asks the loader for a
// function pointer, gets a forwarder back, and registers its replacement
// against that -- so "the original" it is handed to call through would be the
// forwarder, and the first call would recurse until the stack ran out. With
// the pair recorded, DetourAttachEx hands back the real function instead.
void detour_register_forwarder(void* forwarder, void* real);

// How many hooks are registered, for the status line.
std::size_t detour_count();

}  // namespace bg3le
