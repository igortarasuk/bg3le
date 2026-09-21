// See ecs_world.cpp: the ECS storage pointer is captured from a hooked
// component lookup, because the engine exposes no symbol for it.
#pragma once

namespace bg3le {
namespace ecs {

// Patches the call sites of the component lookup so the first call records the
// object it goes through. Returns false if no site could be patched.
bool install_storage_capture();

// The captured pointer, or nullptr if nothing has called the lookup yet.
void* storage();

}  // namespace ecs
}  // namespace bg3le
