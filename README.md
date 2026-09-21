# bg3le

A script extender for the **native Linux build** of Baldur's Gate 3.

The existing Script Extender targets the Windows build, so Linux players run
the game under Proton to get it. bg3le attaches to `bin/bg3` directly.

## Credit

This project stands on [Norbyte's Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se). The game-structure definitions and
the Lua binding framework under `vendor/bg3se/` are theirs (MIT + Commons
Clause); bg3le reuses them rather than rediscovering years of reverse
engineering, and would not be a realistic project otherwise. **Thank you.**

See [vendor/NOTICE.md](vendor/NOTICE.md) for attribution and every change made
to the vendored code — all of it to compile under clang rather than MSVC,
none of it behavioural.

## What works

- Osiris is live: `Osi.*` and the bare-global helpers, callable from an
  interactive prompt while the game runs
- Lua 5.4 host with `Ext.Log`, `Ext.Json`, `Ext.Math` (scalar), `Ext.Table`,
  `Ext.Timer`, `Ext.Utils`, and `_D`/`_P`/`_PW`/`_PE`
- A Lua debugger server compatible with the
  [bg3lua](https://github.com/lenonk/bg3lua) client (`client/` submodule),
  plus `CreateConsole` parity that opens a terminal on startup
- **A 65-98s level load reduced to ~1s.** The native build spends almost all
  of it in `physx::Sn::ConvX` converting PhysX data whose `TempAllocator`
  serialises on one global mutex; `src/fast_alloc.cpp` replaces it with a
  lock-free thread-local pool. See
  [reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md).

## Not implemented

`Ext.Entity` and the other engine-reflection modules, mod loading, and the
client-side modules. The vendored definitions compile, but nothing is wired to
the ECS yet.

## Building

Needs clang, libc++, CMake, oneTBB, protobuf and SDL2.

    tools/fetch-externals.sh    # Noesis, glm, imgui, lua, rapidjson, Vulkan
    cmake -S . -B build && cmake --build build
    tools/check-vendor.sh       # optional: compiles the vendored headers

`libc++` is required rather than optional: the native game is built against it,
so `std::string` is 24 bytes there as here. libstdc++ would give 32 and
silently shift every field after a string in a component.

## Running

    ./run-native.sh

Offsets are pinned to game version `4.8.400.7143220`. `tools/find_slots.py` and
`tools/recover_symbols.py` regenerate them for a new build.

## How it hooks

No Detours and no instruction-length decoder. Three primitives in
`src/hook.cpp` and `src/preload.cpp`:

1. PLT/dynamic-symbol interposition, by mangled name
2. vtable-slot patching — one aligned store, and it verifies the slot's
   current contents first, so a shifted binary is refused rather than corrupted
3. call-site patching — rewrites `call rel32` displacements to a nearby
   trampoline, since rel32 cannot reach a shared library from the executable

Symbols come from the native binary's own `.symtab` (102,920 of them) plus
11,214 recovered from embedded `__PRETTY_FUNCTION__` strings attributed to
their enclosing functions via `.eh_frame_hdr`.

## Licence

bg3le's own code is MIT. `vendor/bg3se/` remains under its upstream MIT +
Commons Clause terms; `vendor/bg3se/LICENSE` applies to it and forbids selling
the software.
