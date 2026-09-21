#pragma once

#include <cstdint>

namespace bg3le {

// Instruments PhysX binary-format conversion. Opt in with
// BG3LE_PHYSX_PROBE=1.
void physx_probe_install();

// Logs conversion counts and total time spent converting.
void physx_probe_report(const char* when);

// Link-time offset -> runtime address in the main executable.
void* physx_resolve(std::uintptr_t offset);

}  // namespace bg3le
