// See ecs_world.cpp: the ECS storage pointer is captured from a hooked
// component lookup, because the engine exposes no symbol for it.
#pragma once

namespace bg3le {
namespace ecs {

// Patches the call sites of EntityStorageContainer::GetEntityStorage so the
// calls record the containers they go through. Returns false if no site could
// be patched.
bool install_container_capture();

// The captured EntityStorageContainers, or nullptr if nothing has gone through
// that slot yet.
//
// There are two, because BG3 runs a client EntityWorld and a server
// EntityWorld with a container each, and which one the engine touches first is
// not ours to choose. Only the server world has replication buffers, so the
// two are told apart by that rather than by capture order -- see
// server_container() in lua_host.cpp.
void* container();
void* container_alt();

// The most recent EntityHandle passed to the lookup. A real, live handle to
// test component access against while UUID -> handle is unimplemented.
unsigned long long last_entity();

}  // namespace ecs
}  // namespace bg3le
