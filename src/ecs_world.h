// See ecs_world.cpp: the ECS storage pointer is captured from a hooked
// component lookup, because the engine exposes no symbol for it.
#pragma once

namespace bg3le {
namespace ecs {

// Patches the call sites of EntityStorageContainer::GetEntityStorage so the
// first call records the container it goes through. Returns false if no site
// could be patched.
bool install_container_capture();

// The captured EntityStorageContainer, or nullptr if no entity lookup has
// happened yet.
void* container();

// The most recent EntityHandle passed to the lookup. A real, live handle to
// test component access against while UUID -> handle is unimplemented.
unsigned long long last_entity();

}  // namespace ecs
}  // namespace bg3le
