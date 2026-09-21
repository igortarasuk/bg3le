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
- Lua host with `Ext.Log`, `Ext.Json`, `Ext.Math` (scalar), `Ext.Table`,
  `Ext.Timer`, `Ext.Utils`, and `_D`/`_P`/`_PW`/`_PE`. The interpreter is
  Norbyte's Lua fork, the same one bg3se uses — see
  [external/lua/README.bg3le](external/lua/README.bg3le) for why that is not
  optional
- Loose-file mod loading: `Mods.<ModTable>`, `Config.json`, `BootstrapServer.lua`
  and `Ext.Require`, discovered via `BG3LE_MOD_PATH`
- `Ext.Entity` against the live ECS: `Ext.Entity.Get(uuid)`, component reads
  and writes, and `entity:Replicate(name)` that reaches the client. The engine
  names every ECS type index in its symbol table, so the component and
  replication registries come straight out of `.symtab` — the Windows extender
  has to recover the same mapping by scanning the image for byte patterns.
  Typed for `Health` so far; see [What is left](#what-is-left)
- A Lua debugger server compatible with the
  [bg3lua](https://github.com/lenonk/bg3lua) client (`client/` submodule),
  plus `CreateConsole` parity that opens a terminal on startup
- **A 65-98s level load reduced to ~1s.** The native build spends almost all
  of it in `physx::Sn::ConvX` converting PhysX data whose `TempAllocator`
  serialises on one global mutex; `src/fast_alloc.cpp` replaces it with a
  lock-free thread-local pool. See
  [reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md).

## What is left

- **Most of `Ext.*`.** Around 265 functions bg3se exposes have no equivalent
  here yet. The ECS plumbing they need is done, so most are now a component
  index plus a vendored struct
- **Aggregate field kinds.** Scalars, enums and fixed-extent arrays are
  converted; `HashMap`, `DynamicArray` and nested structs are not, so
  `ActionResources.Resources` and `SummonContainer.ByTag` read as unsupported.
  Naming one raises rather than returning nil, so a mod cannot mistake a
  missing conversion for a missing value
- **The client-side modules.** `Ext.ClientUI` in particular is blocked on the
  placeholder Noesis RTTI — the native game ships no Noesis typeinfo at all,
  so `src/vendor/noesis_rtti_linux.cpp` aliases 19 of them to one real
  placeholder type. That is safe only while no Noesis `dynamic_cast` runs. The
  real fix is keeping Noesis types out of the generated property maps
- **Launching.** See [Running](#running)

## Building

Needs clang, libc++ (including the static archives), CMake, SDL2 and the
Vulkan loader. protobuf and abseil are built from source by
`tools/fetch-externals.sh` rather than taken from the distribution, because the
packaged builds are compiled against libstdc++ and export `std::__cxx11`
symbols that cannot link into a libc++ library.

    tools/fetch-externals.sh    # Noesis, glm, imgui, lua, rapidjson, Vulkan
    cmake -S . -B build && cmake --build build

Optional checks:

    tools/check-vendor-all.sh      # per-file error counts for vendor/bg3se
    tools/check-vendor-patches.py  # confirms the clang fixes are still applied
    tools/check-prelude.sh         # parses the Lua embedded in lua_host.cpp
    tools/check-array-view.py      # runs the prelude's array view against a stub
    cc -o /tmp/mc tools/meta-check.c -ldl && /tmp/mc build/libbg3le.so
                                   # component field offsets, no game needed

The first four need no game and no built library (`meta-check` needs the
library but not the game). The Lua prelude is a raw string literal, so a syntax
error in it is a runtime failure rather than a build one — hence
`check-prelude.sh`.

**clang is required, not merely supported.** The vendored bg3se sources need
`-fdeclspec`, `-fms-extensions` and `-fdelayed-template-parsing`, none of which
gcc has; CMake fails the configure step with any other compiler.

**libc++ is required too.** The native game is built against it, so
`std::string` is 24 bytes there as here — libstdc++ would give 32 and silently
shift every field after a string in a component. Everything in the library has
to agree on one standard library, so this applies to bg3le's own sources as
well. It is linked statically, for the same reason Lua is vendored: a shim
loaded inside the Steam runtime container cannot rely on host libraries.

## Running

**There is no install or launch story yet.** bg3le is a shared library that
has to be loaded into `bin/bg3` before the engine starts, and arranging that
is an unsolved problem, not a documented step. It needs to work for both Steam
and non-Steam installs, and ideally without the player editing launch options
by hand. Until that exists, running it means knowing how to preload a library
into a process inside the Steam runtime container.

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
