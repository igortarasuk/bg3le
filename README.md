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
to the vendored code — all of it to compile under clang rather than MSVC, none
of it behavioural. Its component definitions have been checked against the
native build rather than assumed: `Ext._Internal.SizeAudit()` compares every
component's declared size with the size the engine recorded, and
`tools/meta-check.c` checks field offsets without needing the game.

## What works

- Osiris is live: `Osi.*` and the bare-global helpers, callable from an
  interactive prompt while the game runs. Out-parameter counts and parameter
  types come from Osiris' own function database rather than from the
  caller's argument count, and the answer is cached under the story version
  — walking it costs 130,000 reads on the story thread otherwise. A name
  with several arities answers to each: Osiris declares `ApplyStatus` with
  three, four and five parameters, the engine's mapping holds only the five,
  and the other two are the story's own, so a count the mapping does not
  have goes the way story functions go
- Lua host with `Ext.Log`, `Ext.Json`, `Ext.Math` (scalar), `Ext.Table`,
  `Ext.Timer`, `Ext.Utils`, and `_D`/`_P`/`_PW`/`_PE`. The interpreter is
  Norbyte's Lua fork, the same one bg3se uses — see
  [external/lua/README.bg3le](external/lua/README.bg3le) for why that is not
  optional
- **Installed mods run.** Mods ship their Lua inside a `.pak`, so bg3le reads
  the archives in the profile's `Mods` directory and the install's
  `Data/Mods`, finds each module by its `ScriptExtender/Config.json`, and
  loads its bootstrap, `Ext.Require` targets and plain `require()` calls
  straight out of the archive. Each mod gets `Mods[ModTable]` as its
  environment with the real globals behind it, and `ModuleUUID` set before
  its table enters `Mods` — Mod Configuration Menu watches that assignment,
  so filling it in afterwards makes every mod look anonymous. Loose
  directories still work, via `BG3LE_MOD_PATH`. Of one 57-mod set, all five
  script mods load and run, MCM included (v1.40.1, "SE version 32")
- **Achievements with mods active**, the way bg3se's `EnableAchievements`
  does on Windows but by a different route: nothing here exports
  `ls::ModuleSettings::IsModded` to patch, so bg3le hooks
  `SteamInternal_FindOrCreateUserInterface`, patches the `ISteamUserStats`
  vtable slot, and calls `SetAchievement`/`StoreStats` itself when Osiris
  dispatches `UnlockAchievement`. Contributed by Igor Tarasyuk; the
  investigation, dead ends included, is in
  [reference/ACHIEVEMENTS-DIAGNOSIS.md](reference/ACHIEVEMENTS-DIAGNOSIS.md).
  `BG3LE_ACHIEVEMENTS=0` turns it off
- **The story's own procedures and databases are callable.** All 19,078 of
  them — `Osi.PROC_*`, `Osi.DB_*` and the story's events, which carry no
  dispatch handle and so cannot go through the DIV boundary at all. They run
  the way the engine runs them: a tuple is inserted into the Rete node the
  function stands for. `Osi.DB_Foo(...)` inserts a fact, `Osi.DB_Foo:Get(...)`
  reads them back with nil as a wildcard, `Osi.DB_Foo:Delete(...)` retracts
  with nil as a wildcard there too, and a bare `PROC_Foo(...)` works as it
  does upstream. Osiris interns its strings, so an argument is interned
  through `COsiStringTable::AddStr` and released afterwards. Nothing here is
  bg3se's offsets: `InsertTuple` is at `+0x68` on this build, not `+0x50`,
  `DeleteTuple` at `+0x70` and not `+0x78`, and the structures were read out
  of the engine's own disassembly —
  see [reference/OSIRIS-STORY-CALLS.md](reference/OSIRIS-STORY-CALLS.md).
  Resolved on first mention rather than at load, as upstream resolves its
  own, so the level load still costs 0.08s
- **`Ext.Osiris.RegisterListener` fires.** `before`, `after`,
  `beforeDelete` and `afterDelete` on any story function, which is how a mod
  watches the game rather than polling it. The two tuple slots are replaced
  in the two node classes that use them — bg3se's `NodeHooks.cpp` does the
  same — and only once a mod subscribes, so until then every node keeps the
  engine's own pointers. Engine-side activity reaches it too: a listener on a
  database sees the fact a procedure's own rule inserts
- **A client Lua context as well as the server's.** The game is two contexts
  in one process and upstream runs a Lua state for each, so bg3le does too:
  each has its own `Ext`, its own `Mods` table, and runs the bootstrap that
  belongs to it. Mod Configuration Menu loads on both sides and prints its
  `[S]` and `[C]` banners; `Ext.IsClient()`/`Ext.IsServer()` answer for the
  state they are asked in. The console switches with `:client` / `:server` —
  the LuaDebug protocol has carried a context on every request all along.
  Osiris is server-side, as upstream has it, and says so in the client
  context rather than blaming the save
- **The engine's own managers found once and remembered.** Everything located
  by content — `RPGStats`, the mod load order, the spell and status
  prototype managers — has the path from a static pointer to it recorded
  under the build id, so later runs dereference instead of scanning. The
  search runs backwards: what points at the manager, what points at that,
  until something in the executable's own writable data does. Story-load
  work went from 30.3s to 0.07s
- **A line on the main menu**, as upstream has: the localisation string is
  patched from the game's own heap the moment it appears, before the
  interface resolves it into its own copy
- `Ext.Entity` against the live ECS: `Ext.Entity.Get(uuid)`, component reads
  and writes, and `entity:Replicate(name)` that reaches the client. The engine
  names every ECS type index in its symbol table, so the component and
  replication registries come straight out of `.symtab` — the Windows extender
  has to recover the same mapping by scanning the image for byte patterns.
  Fields come from bg3se's own generated metadata rather than from accessors
  written per component, so every component it describes is reachable by name;
  see [What is left](#what-is-left) for the kinds that do not convert yet
- `Ext.StaticData.Get`/`GetAll` against the engine's GUID resource manager.
  It has no symbol, so it is found by fingerprint: the manager is one
  `HashMap<StaticDataTypeIndex, GuidResourceBankBase*>`, and a table whose
  keys are all drawn from the 121 static data type indices the symbol table
  already names is that manager rather than a coincidence
- `Ext.Stats`: 15,754 stats, enumerable and readable by name, with every
  attribute kind decoded — ints, floats, strings, GUIDs, enumerations and
  flag sets. `RPGStats` has no symbol and its layout is not ours (our
  `TreasureRarities` sits at 800 where the engine's is at 3648), so nothing
  is read through a member offset: the anchor is seven consecutive
  `FixedString` indices spelling the treasure rarities, and everything past
  it — the stats array, the modifier lists, the value lists, the string,
  int64, guid and float pools, and `Object`'s own field offsets — is located
  by content and validated before use
- `Ext.Mod`, all five functions. The mod manager has no symbol either, so
  the list is found from the one thing every install shares: the base
  module's UUID is the constant `ed539163-…`, which locates a `Module`
  exactly, and the array holding it is the load order. `ModuleInfo` turns
  out to be the Windows struct with Larian's sixteen-byte string in place of
  `std::string` — which is why `sizeof(Module)` upstream does not match the
  240-byte stride in memory — and every field was confirmed against a module
  whose values the reference already records
- **Verified against the real extender.** `reference/` holds output captured
  from a running Windows install over the debugger, and bg3le matches it:
  the same 15,754 stats in the same order, the same attribute values, the
  same proxy object with its bound methods, floats to the digit, and
  `Ext.Mod.GetMod("ed539163-…")` returning a structure equal key for key and
  value for value to `reference/mod-shape.txt`. The
  public API is a compatibility contract — a mod written against bg3se has
  to work here — so it follows the reference rather than convenience.
  `tools/grab-reference.sh` reproduces the capture
- A Lua debugger server compatible with the
  [bg3lua](https://github.com/lenonk/bg3lua) client (`client/` submodule),
  plus `CreateConsole` parity that opens a terminal on startup
- **A 65-98s level load reduced to ~1s.** The native build spends almost all
  of it in `physx::Sn::ConvX` converting PhysX data whose `TempAllocator`
  serialises on one global mutex; `src/fast_alloc.cpp` replaces it with a
  lock-free thread-local pool. See
  [reference/SLOW-LOAD-DIAGNOSIS.md](reference/SLOW-LOAD-DIAGNOSIS.md).

- **A 30fps endgame save brought to 71fps.** BG3's Vulkan backend streams
  per-frame data through the `DEVICE_LOCAL | HOST_VISIBLE` heap. On an
  external GPU that heap is BAR-mapped VRAM across a Thunderbolt hop, where
  CPU writes run at 0.21 GB/s against 14.68 GB/s to host memory, so the main
  thread sat in `memcpy` while the GPU starved at 57%. `src/vulkan_memory.cpp`
  hides `HOST_VISIBLE` from the device-local types so the engine's own
  selection picks host memory: p99 frametime 184ms to 15.8ms, GPU busy to
  99%, and less total CPU. It measures the hardware at startup and does
  nothing on a machine where those writes are fast
- **The engine's thread pinning undone.** It pins each thread to one logical
  CPU, which makes that core the frame gate; the same game under Proton runs
  every thread on `0-15` because Wine ignores the requests, and that build
  never had the problem. `src/affinity.cpp` widens the masks: startup p99
  frametime 133ms to 77.8ms, and no pegged core

## What is left

- **86 of `Ext.*` refuse rather than answer.** Every name bg3se exposes is
  present — `tools/api-coverage.lua` reports 715 of 715 — but the ones
  needing machinery bg3le does not have raise instead of returning a
  plausible wrong answer: stat writes, functor execution, `Ext.Level`'s
  physics and pathfinding, `Entity.Create`/`Destroy`, the atlas and resource
  managers, `GlobalSwitches`, and anything that sends over the network.
  `reference/ext-api-surface.txt` lists them with their shapes
- **One session per process.** The story-load work runs once, so loading a
  second save without restarting leaves Osiris bound to the first story's
  mappings and every mod's script from the first session. Upstream resets
  its Lua state per session; telling a new session from the two or three
  story loads that make up one needs the game state machine, which bg3le
  does not read yet. It says so rather than resetting at the wrong moment
- **`ModManager.Settings`.** It sits past a hash map whose size on this build
  is not established, so `GetModManager` returns `BaseModule`,
  `LoadOrderedModules` and `AvailableMods` and omits it
- **Writing stats.** `Ext.Stats.Get` returns upstream's proxy object with its
  four methods, but `Sync`, `SetPersistence`, `SetRawAttribute` and
  `CopyFrom` raise: writing needs the engine's stat sync path, which is not
  reached yet. They raise rather than no-op so a mod author sees what is
  missing instead of a change that silently does nothing
- **Functors, conditions and requirements inside stats.** Their shapes are
  recorded in `reference/stats-spell.txt` — nested objects carrying a
  `TypeId` — and bg3se exposes them through the same property maps this
  already re-expands for components, so the field machinery should reach
  them once `Object::Functors` is located
- **The last 6% of the field kinds.** 3,344 of 3,558 fields convert
  (94.0%, from `tools/meta-check.c`; the count grew when static data
  resources joined the table and they carry `TranslatedString`): scalars, enums and bitmasks, nested
  structs, fixed and dynamic arrays, hash sets, hash maps, glm vectors,
  `std::optional`, `std::variant` and `FixedString`. What is left is mostly
  `TranslatedString` and raw pointers. Naming an unsupported field raises
  rather than returning nil, so a mod cannot mistake a missing conversion for
  a missing value
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
    tools/check-views.py           # runs the array and map views against stubs
    cc -o /tmp/mc tools/meta-check.c -ldl && /tmp/mc build/libbg3le.so
                                   # field offsets, the container walks, and
                                   # how much of the surface converts

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

`run-native.sh` is the development harness rather than that story. It runs the
game inside the Steam runtime container by default, and `SNIPER=0` runs it
straight on the host — the native binary needs only `libssl.so.1.1` and
`libcrypto.so.1.1`, which `compat-libs/` supplies. Running outside the
container matters for debugging: inside it, libraries are recorded under
`/run/host`, which does not resolve from outside the namespace, and `perf`
can symbolize nothing.

Mods live in `~/.local/share/Larian Studios/Baldur's Gate 3/Mods` with the
load order in `PlayerProfiles/Public/modsettings.lsx`, and one thing about
that will waste a day if you do not know it: the game writes a
`ModCrashSanityCheck` directory into the profile while it runs, deletes it on
a clean exit, and **disables every mod when it finds one left behind**. Kill
the game — as any test harness does — and the next run loads no mods. bg3le
removes it at startup, as bg3se does; `BG3LE_KEEP_SANITY_CHECK=1` keeps it,
which is how that was attributed (14 modules with it, 69 without).
[reference/MOD-LOADING.md](reference/MOD-LOADING.md) has the rest, including
that a savegame's module list replaces the load order.

Offsets are pinned to game version `4.8.400.7143220`. `tools/find_slots.py` and
`tools/recover_symbols.py` regenerate them for a new build. The reference
capture in `reference/` was taken against game `v4.73.98.727`, recorded in
`reference/version.txt` so a later mismatch is attributable.

## Contributing

Patches welcome. Four checks want running before a pull request, all of which
work without the game:

    ./tools/check-symbols.sh        # nothing references an undefined bg3le symbol
    ./tools/check-prelude.sh        # the Lua embedded in lua_host.cpp parses
    python3 client/tools/check-output.py   # the console's terminal handling
    python3 client/tools/check-prompt.py   # prompt width against readline's idea of it

`check-symbols.sh` is the one that matters most: the library links with
undefined symbols allowed, because it has to interpose the engine's own, so a
missing definition of *ours* builds cleanly and then kills the game at the
first call. That has happened three times.

Two conventions worth knowing. Anything located by content is validated
before use — a structure has to agree about something only the real one could
— and a diagnostic that established a layout stays behind an environment
variable rather than being deleted, so the next game patch can re-run it.
`git log` is written to be read; a commit explains why, not what.

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
