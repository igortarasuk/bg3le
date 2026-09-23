# Re-enabling achievements with mods active

Goal: port bg3se's `EnableAchievements` config option (Windows) to bg3le.
Status: **done**. The Linux equivalent doesn't patch out the engine's own
mod-check (its exact location was never conclusively pinpointed -- see
"Session 4" below); instead it forces the real Steam calls directly from
bg3le's own DIV-dispatch hook, bypassing whatever the engine's internal
handler decides. Live-validated end-to-end: with mods active, calling
`Osi.UnlockAchievement(name, character)` now produces an immediate, real
Steam achievement toast. Implementation: `src/preload.cpp`,
`maybe_force_unlock_achievement` and the "ISteamUserStats vtable hook state"
block above `call_wrapper`.

## What bg3se (Windows) actually patches

`BG3Extender/GameHooks/DataLibraries.cpp`, `LibraryManager::ApplyCodePatches()`:

```cpp
if (gExtender->GetConfig().EnableAchievements && !WasPatchApplied("ls::ModuleSettings::IsModded")) {
    if (ApplyCodePatch("ls::ModuleSettings::IsModded") && ApplyCodePatch("esv::SavegameManager::ThrowError")) {
        DEBUG("Modded achievements enabled.");
    }
}
```

Two independent single-branch patches, found by AOB pattern (not by symbol --
neither function is exported even on Windows):

**`ls::ModuleSettings::IsModded`** (`BinaryMappings.xml`):
```
@str 3B 05 ?? ?? ?? ??   ; cmp eax, cs:dword_XXX
74 05                    ; jz short skip
@ref 40 b7 01            ; mov dil, 1      <- patched to `40 32 ff` (xor dil, dil)
eb 03                    ; jmp short set
40 32 ff                 ; xor dil, dil
83 f8 ff                 ; cmp eax, -1
```
Condition anchor: a `FixedString` reference to
`d65cf1b6-23a8-f0db-0a56-4b479a559748` earlier in the same function. The patch
forces the "modded" branch to always produce `dil = 0` (not modded).

**`esv::SavegameManager::ThrowError`**: after
`ls__ModuleSettings__HasCustomMods()` returns, a `jz` that would otherwise
throw/warn on custom mods gets NOP'd (`66 90`).

Both patches are 1:1 same-size byte swaps, not a shift -- no relocation to
worry about, which is exactly the pattern bg3le's own `hook.cpp` (vtable-slot
patching, call-site patching) is built to do.

## What `ls::ModuleSettings::IsModded` actually computes

From `vendor/bg3se/BG3Extender/GameDefinitions/Module.h`:

```cpp
struct ModuleSettings : public ProtectedGameObject<ModuleSettings> {
    void* VMT;
    Array<ModuleShortDesc> Mods;   // currently active mod/module list
};
```

There is no stored bool -- `IsModded()` is a method that walks `Mods` and
compares each `ModuleUUID` against a small fixed set of official module GUIDs
(base game, its dev variant, Shared, SharedDev, etc.). `HasCustomMods` is the
same idea from `esv::SavegameManager`'s side.

`ModuleSettings` lives embedded in `ModManager`, which lives embedded in
`EoCServer` (`&server->ModManager`, not a pointer -- confirmed by
`GetModManagerServer()` in `vendor/bg3se/BG3Extender/GameDefinitions/Symbols.inl`,
and by the Windows `ThrowError` disassembly doing `mov rcx, cs:esv__gEocServer;
add rcx, 108h` to reach it directly).

## Confirmed on the native Linux binary (`bin/bg3`, build matches
`reference/recovered-symbols-4.8.400.7143220.txt` byte-for-byte -- see below)

- The literal GUID `d65cf1b6-23a8-f0db-0a56-4b479a559748` **is present**,
  once, at file offset `0x1a5631f` (`.rodata`, addr == offset for this
  binary).
- Its only code reference is a `lea rsi, [rip+...]` at `0x40905ca`, which
  turned out to be inside one enormous (0x56b0-byte, `0x408d2f0`-`0x40929a0`)
  anonymous TU-constructor that sequentially builds several `FixedString`
  constants (ours is one of six built back-to-back: `.L.str.733` through
  `.L.str.738`) into unnamed statics elsewhere in `.data`/`.bss`. This is
  eager static initialization of the "known official module GUIDs" table,
  **not** the comparison site itself -- a dead end for direct patching.
- `nm -C --defined-only bin/bg3` has **no** symbol for `ls::ModuleSettings::`
  anything, `HasCustomMods`, `ModManager`, `EoCServer`, or any RTTI
  (`typeinfo for esv::EoCServer` / `vtable for esv::EoCServer` -- absent, so
  the binary is `-fno-rtti` or RTTI got stripped). All of `IsModded`,
  `HasCustomMods`, and the `EoCServer`/`ModManager` globals are fully inlined
  with no exported name, same as `EntityStorageContainer` was (see
  `ecs_world.cpp`'s header comment) -- this needs the same
  disassemble-a-stable-pattern-and-verify approach that file used, not a
  symbol lookup.
- Four *named* functions do take `ls::ModuleSettings const&` as a parameter
  (found via `recover_symbols.py`, real hits, real addresses):
  `esv::LoadProtocol::LoadModule(ls::ModuleSettings const&, bool)` @
  `0x6efd8b0`, `LoadSavegame` @ `0x41e6ee0`, `HandleModuleLoaded` @
  `0x40150b0`, `LoadModuleAndLevel` @ `0x41e7760`. These are legitimate hook
  points if the goal shifts from "patch the branch" to "observe/override the
  struct once it's loaded."

## Version compatibility -- turned out to be moot

`tools/recover_symbols.py` run against this machine's `bin/bg3` produced a
symbol map **byte-for-byte identical** (all 9427 entries, same addresses) to
`reference/recovered-symbols-4.8.400.7143220.txt`, which was committed
2026-09-20. So despite a stray `4.1.1.7398727` version string elsewhere in
the binary (likely unrelated metadata, not the build id), this installs is
the same build bg3le already targets. No offset regeneration needed.

## Live LD_PRELOAD attach: confirmed working end-to-end

Launching via Steam with `LD_PRELOAD=.../libbg3le.so %command%` in Launch
Options (launching the sniper `run` wrapper *directly*, bypassing Steam,
gets the game to restart itself through Steam and drop the preload --
apparent `SteamAPI_RestartAppIfNecessary`-style behaviour; going through
Steam's own launch avoids it) gets a fully working session:

```
elf: parsed 102920 symbols
ecs: 13772 type indices (2107 components, ...)
LUA VM initialised (Lua 5.3.6)
Hooked server tick via slot 0x7a88228
Debug server listening on 127.0.0.1:9998
StoryLoaded(): 1303 Osiris functions, 39 types
Bound 983 Osiris functions as Osi.* and globals
```

The debug server (`src/debug_server.cpp`) speaks a small length-prefixed
protobuf-lite protocol (framing: 4-byte LE total-size prefix; fields 1=seq,
3=Connect, 7=Evaluate-in/5=Evaluate-out, 8=async log). A ~150-line Python
client (no dependencies) is enough to drive it as a Lua REPL against the live
game -- useful for anything past this point instead of more blind static
disassembly. `os`/`io` are unrestricted from that Lua VM (`os.execute`,
`io.popen` both work), which is handy for further probing.

`Ext.Mod` is currently empty (`{}`) -- `ModManager`/`EoCServer` access isn't
implemented yet, consistent with them having no name to resolve by.

## `EoCServer` found -- the vendored offsets are correct after all

First attempt was wrong in an instructive way: `preload.cpp`'s server-tick
hook (`update_messages_hook`, `kUpdateMessagesFunc = 0x7077120`) does **not**
receive `esv::GameServer*` despite the existing comment above it -- its `self`
disassembles to a mutex-protected ring-buffer flush (fields at
`+0x790..+0x7d0`, a `pthread_mutex_t` at `+0x7a8`), i.e. some low-level
outbound-message queue, not the class in `GameDefinitions/Net.h`. Computing
`EoCServer = self - 168` from that gave garbage (`Mods.count` in the billions).
The offsets themselves were not the problem -- the pointer was.

The real path, found by disassembling `esv::LoadProtocol::LoadSavegame`
(`0x41e6ee0`) past its entry (which is mostly a contract-check preamble
referencing its own `__FUNCTION__` string -- confirms the address is
genuinely that function):

```
41e7486: mov  r12, QWORD PTR [rip+0x39a536b]   ; r12 = *(bg3+0x7b8c7f8) -- a real global pointer, EoCServer*
41e748d: lea  rdi, [r12+0xd0]                  ; rdi = EoCServer + 0xD0 == ModManager (matches offsetof exactly)
41e7495: mov  rsi, r15
41e7498: call 0x4285940
41e749d: add  r12, 0x268                       ; r12 = EoCServer + 0x268 == ModManager+0xD0+Settings+0x198, i.e. &ModuleSettings
41e74a4: test r14, r14                         ; r14 = the settings-argument (often NULL, see below)
41e74a7: cmovne r12, r14                       ; ...only overridden when a non-null one was actually passed in
```

`0xD0 + 0x198 = 0x268` exactly, so this independently confirms
`offsetof(EoCServer, ModManager) = 0xD0` and `offsetof(ModManager, Settings) =
0x198` from `tools/offset_dump.cpp` (see below) -- the vendored struct
layouts are correct for this binary; the earlier failure was purely a wrong
`self` pointer, not a wrong offset.

**`bg3+0x7b8c7f8` (add the session's load bias) holds a live, dereferenceable
`EoCServer*`, independent of any hook.** Verified end-to-end from the debug
console with no gdb involved, just `/proc/self/mem` reads from Lua:

```
g_root_loc = bias + 0x7b8c7f8
eocserver  = *(u64*)g_root_loc
modsettings = eocserver + 0x268
mods_buf   = *(u64*)(modsettings + 8)     -- Array<ModuleShortDesc>::buf
mods_count = *(u32*)(modsettings + 16)    -- Array<ModuleShortDesc>::size
```

Result on this machine, in an active session: `mods_count = 12`. Dumping the
96-byte `ModuleShortDesc` entries (`Guid ModuleUUID` at `entry+8`, matching
`offsetof(ModuleShortDesc, ModuleUUID) = 8`; `STDString Name` follows at
`entry+24`, readable inline via libc++ SSO for short names) shows real,
sane data -- entry 0 is `GustavX` (an official module), entry 3 is
`FaerunColors` (an actual installed Nexus mod on this machine). This is
about as strong a confirmation as static analysis can give without a
debugger: the whole `EoCServer -> ModManager -> Settings -> Mods` chain is
real and correctly offset.

`tools/offset_dump.cpp` (new, throwaway, not part of the CMake build) gets
these numbers straight from the vendored headers via a deliberate
`Show<offsetof(...)>` template-instantiation error, sidestepping the link
failure `GameState.h`'s Noesis dependency causes in a standalone TU:

```
sizeof(esv::EoCServer) = 696         EoCServer::EntityWorld  = 648
EoCServer::GameServer  = 168         sizeof(ModManager)      = 432
EoCServer::GameStateMachine = 160    ModManager::Settings    = 408 (0x198)
EoCServer::ModManager  = 208 (0xD0)  ModManager::BaseModule  = 56
sizeof(ModuleSettings) = 24          ModuleSettings::Mods    = 8
sizeof(ModuleShortDesc) = 96         ModuleShortDesc::ModuleUUID = 8
```

## What did *not* work: catching the actual read/comparison live

Wanted a hardware watchpoint or breakpoint to catch the code that actually
computes "is this session modded" red-handed, instead of guessing at the
byte pattern. None of the following fired, across a real, active game
session with `sudo gdb -p <pid>`:

- **`rwatch`** on the static official-GUID table entry found earlier (the
  hashmap/tree-node-shaped live copy of `d65cf1b6-...`, found by a chunked
  `/proc/self/mem` scan for its raw 16-byte encoding rather than the far too
  common ASCII "Gustav") -- survived opening the save/load menu and a Quick
  Save untouched.
- **`rwatch`** on the live `Mods.count` field itself (`modsettings+16`) --
  survived two Quick Saves.
- **`break`** on `esv::LoadProtocol::LoadSavegame` (`0x41e6ee0`) -- fired
  twice (once from an in-session Load, once from a cold main-menu Load), but
  its third argument (the nullable `ModuleSettings const*` override, `r14` in
  the disassembly above) was `NULL` both times, meaning this call site relies
  on the `EoCServer`-derived default rather than passing settings explicitly.
- **`break`** on `esv::LoadProtocol::LoadModule` (`0x6efd8b0`) -- never fired,
  neither for a cold main-menu Load nor for opening the in-game Mods screen
  (which does visibly spawn ~16 worker threads, so it does real work, just
  not through this address).

Working theory: whatever produces the modded/not-modded verdict runs once,
early, at true session bootstrap (first load after the process starts, or
character creation) and the result gets cached -- reloading the *same*
already-established session, or browsing the Mods screen, doesn't
recompute it. Everything tried above was against an already-running,
already-loaded session.

## Fresh-process load: still only `LoadSavegame`, not the other three

Relaunched the game from scratch and set breakpoints on all four
`ModuleSettings`-taking functions simultaneously, *before* touching the main
menu's Continue/Load at all. Only `LoadSavegame` (`0x41e6ee0` + bias) fired
-- `LoadModule`, `HandleModuleLoaded`, `LoadModuleAndLevel` never did, even
for the very first load of the process's life. So the "cached, computed once"
theory was half right and half wrong: `LoadSavegame` *is* the real entry
point for a normal single-player Continue/Load (it must read the module list
out of the save file itself rather than needing it passed in, hence the
`ModuleSettings const*` argument being reliably `NULL` -- that parameter is
an override path, not the normal one). The other three are for something
else (fresh campaign creation via the main menu's "New Game", most likely).

Dumped `esv::LoadProtocol::LoadSavegame` in full via `objdump` (it is large;
spans at least `0x41e6ee0`-`0x41ec000`) and grepped for the two most useful
signal types:

- References to the known official-GUID literal addresses
  (`.rodata` addresses of `.L.str.733`-`.L.str.738`, the same six built by
  the static-init table from earlier): **zero** hits in the function. Expected
  -- a real check would compare against the already-hashed/interned form, not
  re-reference the raw string.
- `setcc` instructions (`sete`/`setne`/`setb`, i.e. "turn a comparison into a
  0/1 byte" -- the compiler's typical output for computing a bool, and what
  the Windows patch target ultimately does too): only **4** in the whole
  function.
  - `sete sil` @ `0x41e7e52`: gated behind `EoCServer+0xB0` (not
    `ModuleSettings`), building a UI dialog call with two 16-bit literal
    operands (`0x5ad5`, `0x5ade` -- look like localization/string-table IDs)
    and comparing a state byte to 3. Looks like unrelated loading-phase UI,
    not a mods check.
  - `setne cl` @ `0x41e81aa` and `setb bl` @ `0x41ebd88`: both inside
    obviously unrelated code (one next to a `net::AbstractPeer::BindSocket`
    `__FUNCTION__` string).
  - `sete al; xor al,0x1` @ `0x41eafb2`-`0x41eafbd`: this one is interesting
    on its own merits -- it is a **generic 16-byte struct equality comparator**
    (`return a != b`, byte-for-byte, on two 16-byte blocks -- Guid-shaped),
    but it is a tiny leaf utility that could be called from anywhere in the
    binary for any GUID comparison, not evidence of anything by itself.
    Worth revisiting by finding *its* callers specifically (not by grepping
    this one function) if the direct approach below stalls.

None of the four are a clean match. The actual "modded" branch may not live
inside `LoadSavegame` at all -- it plausibly runs later, once inside an
active session (an Osiris/Lua signal, or on the first Steam achievement
attempt itself), rather than at load time.

## Next step

Two options, in order of directness:

1. **Watch the achievement path itself instead of the mod-list path.**
   `Osi.UnlockAchievement`/`Osi.ProgressAchievement`/`Osi.SetAchievementProgress`
   are real, bound Osiris externals (`Ext._Internal` confirms `Osi.UnlockAchievement`
   exists and is callable from the live console). Their native implementation is
   the actual place the modded-check has to happen for achievements
   specifically, regardless of where `IsModded` itself lives. Not yet tried
   live: calling `Osi.UnlockAchievement(...)` from the debug console with no
   active game session (main menu, no character/story loaded) risks a crash
   from missing preconditions, so this needs an active session with a real
   character handle first, not another main-menu test.
2. Set the same four-breakpoint net (plus an `rwatch` on `EoCServer+0x268+16`,
   i.e. the live `Mods.count`, cheap to compute now that `bg3+0x7b8c7f8` is
   known) **inside an active session** rather than at the main menu --
   attempt an actual in-game action that plausibly re-derives modded status,
   e.g. opening the achievements panel if the Steam overlay exposes one, or
   completing a small in-game milestone that would normally pop an
   achievement.

## Confirmed live: the block is real, and `LoadAchievementsDisabled`'s enum value is known

From the debug console, with a loaded save (host character
`2c8e709e-01da-5c94-c989-475872f32125` fetched via `Osi.GetHostCharacter()`):
called `Osi.UnlockAchievement("NEW_ACHIEVEMENT_1_0", host)` through the normal
Osiris dispatcher (same path a story script uses, not a raw pointer call --
low risk, and it didn't crash). No Steam achievement toast appeared. Not
conclusive on its own (the achievement name is a guess -- BG3's Steam schema
only exposes generic `NEW_ACHIEVEMENT_1_N` display-string keys via
`~/.local/share/Steam/appcache/stats/UserGameStatsSchema_1086940.bin`, not
necessarily the real API names), but consistent with everything else found
here: the game does gate achievements while mods are active, same as on
Windows.

Separately, the save-slot UI badge shown for a modded save (the user noticed
it directly in the Load-game list -- a strong, correct call: this runs when
the save list is *populated*, not when a specific save is *loaded*, which is
exactly why nothing hooked in `LoadSavegame` ever fired) uses the string
`LoadAchievementsDisabled`, found once in `.rodata` at `0x19584fd`. Traced its
only code reference to a large (0x3b1b340-0x3b1bde0) generic
"numeric-id -> localized-string, with an LRU-ish cache" dispatcher --
this is Noesis UI's string-resolution helper, used for hundreds of different
UI messages throughout the game, not achievement-specific by itself.

Reconstructed its multi-tier jump-table dispatch by hand and confirmed by
brute-force byte-scanning each of the 4 candidate tables
(`0x1bd4298`/`0x1bd4498`/`0x1bd44c8`/`0x1bd4804`) for a slot whose relative
offset resolves to `0x3b1b5b4` (the `LoadAchievementsDisabled` case):

**Enum value `144` (`0x90`) selects `LoadAchievementsDisabled`** -- match at
table `0x1bd4298`, index `44` (`144 - 100 = 44`, matching the range check
`cmp eax,0x63; jle ...; add eax,-100; cmp eax,0x7f; ja error` seen in the
dispatcher's own range-selection code).

The dispatcher's only 2 direct callers (`0x30f77ef` passing `144+189=` no --
they pass `0x133`/307 and `0x2c1`/705, neither is 144) are unrelated error
paths elsewhere in the engine. No `lea`-based function-pointer reference and
no raw 8-byte pointer to the dispatcher's own address exist anywhere in
`.rodata`/`.data`/`.data.rel.ro` either, so whatever supplies `144` is not a
literal in any single x86 instruction we can grep for -- it is very likely a
**data-driven value** (a lookup table indexed by save-slot state, or a Noesis
XAML/property-binding enum), which static grepping for an immediate operand
cannot find. This is the actual reason the search stalled here, not a dead
end in the reasoning -- the next session should look for where `144` (or a
small integer in that neighbourhood) sits as *data* rather than as an
instruction operand, e.g. scanning `.rodata`/`.data.rel.ro` for `0x90`
adjacent to the other save-slot-state enum values used by this same
dispatcher (`0x133`, `0x2c1`, and whatever the "corrupted save" / "wrong
version" cases turn out to be), which would locate the enum table itself
rather than one instruction referencing one entry of it.

## Session 2: Steam vtable hook (works, but doesn't answer the question yet)

Added a real, permanent diagnostic to `src/preload.cpp`: bg3's own PLT has
**zero** relocations against the flat `SteamAPI_ISteamUserStats_*` functions
(confirmed via `readelf -r`) -- it is a normal C++ Steamworks SDK consumer,
not a flat-API one. It gets its `ISteamUserStats*` through
`SteamInternal_FindOrCreateUserInterface(hSteamUser, "STEAMUSERSTATS_INTERFACE_VERSION012")`
(this symbol *is* PLT-imported, confirmed live) and calls straight through
the interface's vtable. `libsteam_api.so`'s own flat-API thunks are just
`jmp [rax+offset]` on that same object, so disassembling them gives the exact
vtable slot for free: `SetAchievement` = `+0x38` (slot 7), `StoreStats` =
`+0x50` (slot 10).

`preload.cpp` now interposes `SteamInternal_FindOrCreateUserInterface`
(case-insensitive match on `"UserStats"` in the version string -- the real
constant is all-caps, a first attempt missed it by matching mixed case), and
on the first `ISteamUserStats` interface it sees, patches vtable slot 7 with
a logging trampoline (`mprotect` the single 8-byte slot, swap the pointer,
`mprotect` back), the same primitive family as `hook.cpp`'s `hook_slot`. This
**works** -- confirmed live, logs `steam hook: ISteamUserStats vtable ...
SetAchievement slot -> ...`.

Two attempts to actually trigger it:
- `Osi.UnlockAchievement("NEW_ACHIEVEMENT_1_0", host)` from the debug
  console, both with and without mods active: call returns cleanly, **but
  never reaches the hooked `SetAchievement`**. Root cause: `NEW_ACHIEVEMENT_1_0`
  is not a real achievement ID -- it is only the localization *token* BG3's
  Steam schema uses for the display name/description (confirmed by parsing
  `~/.local/share/Steam/appcache/stats/UserGameStatsSchema_1086940.bin`
  directly: real API names are `BG3_Quest01`..`BG3_Quest54`, e.g. `name` field
  = `BG3_Quest01` for the "Descent from Avernus" achievement). The engine
  presumably rejects the unknown name before ever touching Steam, so this
  result says nothing about the mod block either way.
- `Osi.UnlockAchievement("BG3_Quest01", host)` (a real, already-earned-by-this-save
  achievement ID, chosen specifically to be safe) **crashed the game**
  (SIGSEGV, confirmed via `coredumpctl`). Backtrace root-caused to
  `bg3le::osi::invoke()` -> `COsiArgumentDesc::SetAnyString()` -- a marshalling
  bug in `src/osi.cpp`, not in the Steam hook (the crash happens before
  `invoke()`'s `handler()` call is ever reached, i.e. before Osiris itself
  runs). Initial hypothesis (the `NextParam`-pointer memcpy in `invoke()`'s
  per-param loop overwriting the value fields it just wrote) was checked
  against `vendor/bg3se/BG3Extender/GameDefinitions/Osiris.h` and is **wrong**
  -- `NextParam` really is the first 8-byte field there, matching what
  `osi.cpp` assumes, so the bug is something else (most likely: `UnlockAchievement`
  is one of the Osiris functions whose parameter signature bg3le's
  `load_out_param_counts` signature walk failed to resolve -- see "Signature
  walk found ... matched 324 of 1303 functions" in the boot log -- so its
  `fn.params` may have the wrong count/types/order). **Not fixed this
  session** -- deliberately parked in favour of the direct static hunt below,
  since fixing it is a separate, real bg3le bug worth its own investigation
  rather than a blocker for the achievements question specifically.

Net result: the Steam-side hook infrastructure is sound and reusable, but we
still have no live confirmation of where the block sits, because the one
reliable way to *drive* a real achievement call through it (`Osi.UnlockAchievement`
with a real ID) currently crashes.

## Session 2: static hunt for the comparison, round 2 -- also inconclusive

Picked up the "worth revisiting by finding its callers" lead on the generic
16-byte comparator at `0x41eafa0` (`sete`+`xor`+SIMD-fallback shape, i.e.
almost certainly `FixedString::operator==` or equivalent -- a leaf utility
used all over the engine, not achievement-specific by itself). Found its
function boundary cleanly this time (`int3` padding before/after, unlike the
generic-registration functions below) and its exact 6 direct (non-inlined)
call sites binary-wide via an `E8 rel32` scan:

- `0x3d2ac85`, `0x5f82b6d`: not yet examined.
- `0x41ea86a`, `0x41ead58`, `0x41ead8a`, `0x41eb231`: all four sit physically
  inside/near the `LoadSavegame` address range, but disassembling the
  surrounding code shows they are all comparing against a single constant
  address (`0x1e95040`), with vector-growth/index-store code around them
  (`+0x18`/`+0x30`/`+0x38`/`+0x3c` field pattern) -- this is a generic
  `Set<FixedString>::Insert`-shaped hash-set operation (comparing against an
  empty-slot sentinel), physically link-adjacent to `LoadSavegame` by
  coincidence, not semantically part of it. Another dead end.

Also tried the direct approach of searching for a function that references
**several** of the six official-GUID globals at once (the actual comparison
would plausibly need to check a mod's UUID against all six). Found two such
clusters by raw `lea`-target scanning
(`0x4ca37ef`-`0x4ca3867` and `0x740ac11`-`0x740ac6b`, each hitting all 6
targets) -- both turned out to be generic "for every static global in this
translation unit, call a shared per-object registration helper"
(`__cxa_atexit` in one case, an unidentified `call 0x3883660` in the other)
loops that touch literally every static in the binary, not anything
achievement-specific. A third variant (32-bit `mov reg,[rip+addr]`, i.e.
*reading* a target's value rather than taking its address) found 20 hits, but
all against just two of the six targets (`0x7d26444`, `0x7d25a38`), scattered
across a ~1.5MB code range -- too generic-looking to be the check (more
likely some frequently-read flag/type-tag unrelated to mods).

**Conclusion: raw objdump+python byte-pattern xref scanning has been pushed
as far as it usefully goes on this binary.** The false-positive rate is high
because BG3's binary is enormous and heavily templated (generic containers
get instantiated and inlined everywhere), so "who references address X"
answered by opcode-pattern-matching keeps surfacing generic
container/registration machinery instead of business logic.

## Session 2: Ghidra setup (portable, no sudo) and headless hunt

Installed both without touching the system (no `pacman -S`, no sudo needed at
all):

- **JDK**: `pacman -Sp jdk-openjdk` prints the resolved mirror URL without
  installing anything; downloaded that `.pkg.tar.zst` directly and extracted
  it with `tar --use-compress-program=unzstd` into
  `tools/jdk_extract/`. One fixup needed: the package's
  `usr/lib/jvm/java-26-openjdk/conf` is a symlink to `/etc/java-openjdk`
  (which doesn't exist without a real package install) -- replaced it with a
  symlink to the package's own extracted `etc/java-openjdk` instead.
- **Ghidra**: latest GitHub release zip, extracted with Python's `zipfile`
  (no `unzip` binary on this system) -- note this does **not** preserve the
  executable bit, so every script under `support/` needed an explicit
  `chmod +x` pass afterwards.
- **Import + full auto-analysis**: `analyzeHeadless <project> bg3proj -import bin/bg3`
  took **~1h55m** single-threaded (confirmed via `top`: one core at ~100%,
  the other 19 idle -- Ghidra's core auto-analysis pipeline does not
  parallelize across analyzers) on this ~90MB-`.text` binary. Produced a very
  large number of `WARN (ClearFlowAndRepairCmd) Removing function with bad
  body` / `WARN (MultEntSubModel) Failed to find entry point for subroutine`
  messages throughout -- Ghidra's own function-boundary analysis clearly
  struggled with this binary (likely the same heavy SIMD/inlining/-O2
  codegen that made raw disassembly hard by hand).
- **Gotcha**: `-process <name>` **re-runs full auto-analysis by default**
  unless `-noanalysis` is also passed -- burned ~15 minutes re-analyzing an
  already-analyzed project before this was caught and killed/restarted
  correctly.
- **Gotcha**: Ghidra 12.1.4 dropped the bundled Jython interpreter; `.py`
  post-scripts now require a real PyGhidra (CPython bridge) install, which we
  don't have. Rewrote the post-script in Java instead
  (`tools/ghidra_scripts/FindIsModded.java`, a plain `GhidraScript`) --
  works with zero extra setup.

**Findings from the Ghidra xref dump (`bg3le/reference/ghidra_findings.txt`,
committed alongside this file) are sparse and inconclusive**, and notably
*weaker* than the manual objdump scan:

- Only 3 distinct functions found referencing any of the six official-GUID
  globals at all (`FUN_04f496f0` hitting 2/6, `FUN_047de360` and
  `FUN_04797ec0` hitting 1/6 each) -- all with `refType=WRITE`/`DATA`, i.e.
  these read as *construction* sites, not comparison sites. None of these
  three addresses match the `0x408d2f0` constructor found by hand earlier,
  so Ghidra appears to have found a **different** set of writers than the
  ones identified manually -- unreconciled.
- **Decompilation failed for all three** (empty error message from
  `DecompInterface`), which combined with the huge number of "bad function
  body" warnings during import suggests Ghidra's control-flow/function
  analysis genuinely did not fully map this binary's code -- likely needs
  non-default analyzer settings (or per-function manual re-analysis at the
  addresses already known from objdump) to be more useful than the manual
  approach was. Not investigated further this session.

**Bottom line for that session**: Ghidra is installed and working
(`tools/ghidra_project/bg3proj`, `tools/ghidra_scripts/FindIsModded.java`),
but its default-settings automated analysis of this specific binary is not
yet better than manual objdump work, just different.

## Session 3: the block is now conclusively located (inside `UnlockAchievement`'s native handler)

First, resolved the open question from session 2 about `UnlockAchievement`'s
real signature *without guessing*: bg3le already dumps every Osiris
function's real name/id/param types straight from the engine's own function
table on every launch, unconditionally, to `/tmp/bg3le-osi.<pid>.txt`
(`BG3LE_OSI_DUMP` env var to override the path) -- this was sitting right
there in `preload.cpp` the whole time. It confirms:

```
func 0x80001669 UnlockAchievement(STRING, CHARACTER)
```

-- exactly the shape we'd been calling it with (`Osi.UnlockAchievement(name, host)`).
So the earlier signature-mismatch theory for the `osi.cpp` crash was wrong.
Also found in the same dump that BG3's *real* Steam achievement API names are
`BG3_Quest01`..`BG3_Quest54` (confirmed independently by parsing
`UserGameStatsSchema_1086940.bin`'s `name` field, not just its display-string
tokens) -- `NEW_ACHIEVEMENT_1_N` are display-string tokens only, never valid
arguments to `UnlockAchievement`.

Re-tested `Osi.UnlockAchievement` with **the correct signature, real IDs,
carefully chosen values**:

- `Osi.UnlockAchievement("BG3_Quest01", host)` (already-earned-by-this-save
  achievement) -- still **crashes**, reproducing session 2's SIGSEGV in
  `COsiArgumentDesc::SetAnyString`. Root cause still unconfirmed, but now
  narrowed: it is specific to *re-triggering an already-unlocked* achievement,
  not to real names or the `(STRING, CHARACTER)` shape in general (see next
  point). Parked again -- a real, separate bg3le bug, not a blocker.
- `Osi.UnlockAchievement("BG3_Quest54", host)` (a real, not-yet-earned
  achievement -- "Unbreakable Hammer") -- **does not crash**, in either mod
  state, and gives a clean, repeatable, conclusive result:
  - **Mods active** (12 loaded modules, confirmed live -- see below): call
    returns normally, but the Steam vtable hook (from session 2) never fires.
    No error, no crash, no toast. Silent no-op.
  - **Mods inactive** (1 loaded module, confirmed live, same save): call
    returns normally, **the vtable hook fires**
    (`ISteamUserStats::SetAchievement -> true` in the log), **and the
    achievement actually unlocked in Steam** (user confirmed the "Unbreakable
    Hammer" toast/unlock live).

**This is the conclusive result the whole investigation was after.** The
mod-block sits somewhere *inside the native code path that
`UnlockAchievement` (Osiris function `0x80001669`) dispatches to*, strictly
before any Steam call -- not in a separate, later check, not something that
only affects the save-list UI badge. Confirms the bg3se analogy exactly:
same shape as `ls::ModuleSettings::IsModded` gating achievement-relevant
calls on Windows.

Also fixed, in passing: the `ModuleSettings::Mods` container is **not**
`{begin, end, capacity}` (std::vector-shaped) as assumed all through session
1 -- it is bg3se's own `Array<T>` ABI, `{void* Begin; uint32_t Size;
uint32_t Capacity;}` (confirmed live: reading `settings+16` as one 8-byte
value and splitting it into two 32-bit halves gives sane, matching
Size==Capacity numbers, `12` with mods active and `1` without). The
*offsets* used throughout session 1 (`Settings=0x198`, `Mods=8`,
`ModuleUUID=8`, stride 96) are still correct -- only the *second and third
fields'* interpretation was wrong. Session 1's live mod-name reads happened
to work anyway because they only ever used `Begin` + stride, never `End`.

### Next step: find `UnlockAchievement`'s native handler directly

This changes the search from "find the IsModded comparison somewhere in a
90MB `.text`" to a much narrower one: **find the single native function
Osiris function id `0x80001669` dispatches to**, and read the mod check
directly out of it. Two ways in, not yet tried:

1. The Osiris function table bg3le already reads (`get_funcs`/`MappingInfo`
   in `preload.cpp`, feeding the `/tmp/bg3le-osi.*.txt` dump) may carry a
   handler function pointer per entry alongside id/name/params -- check
   `MappingInfo`'s full field layout (`preload.cpp`/wherever it's declared)
   for anything pointer-shaped besides `param_types`, and if present, read it
   live for id `0x80001669` the same way the debug console already reads
   other struct fields.
2. Failing that: `COsiris::Event`/the DIV dispatch path bg3le already
   interposes (`_ZN7COsiris5EventEjP16COsiArgumentDesc` in `preload.cpp`)
   must eventually reach this handler through the engine's own function
   table -- a live `rwatch`/`break` on that specific dispatch, filtered to
   event/call id `0x80001669`, would land directly in the right function on
   the very next real or synthetic `UnlockAchievement` call (once the
   `osi.cpp` crash on repeat-unlock is understood well enough to make that
   safe, or by using a fresh not-yet-earned id each time to sidestep it).

## Session 4: found the dispatch mechanism, gave up on the branch, bypassed it

Followed the two leads above, then abandoned patching the internal check in
favor of forcing the real Steam calls directly. Details below; the short
version is in the Status line at the top of this file.

### Locating `UnlockAchievement`'s native handler

`BG3LE_WRAP_DIV=1` (see `preload.cpp`, `maybe_wrap_div_table`) intercepts the
two function pointers (`copy[1]`/`copy[2]`, "Call"/"Query") the game hands to
`COsiris::RegisterDIVFunctions`, giving `g_real_call`/`g_real_query` -- the
generic native entry points Osiris itself dispatches *through* for every
`Osi.*` function, story-script or Lua alike. Live gdb breakpoint-chaining from
there (conditional breakpoints, chained via `commands` blocks that enable the
next one and `continue`, load-bias recomputed from `/proc/<pid>/maps` each
relaunch since ASLR moves everything) found:

- A flat dispatcher (static offset `0x2cd4500`) that takes the Osiris
  function id in `rdi`, indexes `(id>>3)&0x1ffffff` into a global array of
  per-function descriptor objects, walks the argument chain doing type
  validation, then does a C++ virtual call (`call [rbx_vtable+0x10]`) to the
  function's actual native implementation.
- For id `0x80001669` (`UnlockAchievement`), that virtual call lands on a
  this-adjusting thunk at static offset `0x3088b70`, which falls through to
  the real handler body at `0x3088b90`.

An exhaustive breakpoint sweep of every `call`/`jmp` site in
`0x3088b70`-`0x3089a00` (19 sites, `objdump` + a small Python script to
generate the gdb batch file, each breakpoint incrementing a shared counter
and logging its own address before auto-continuing) gave the real execution
path for a fresh, not-yet-earned achievement id with **mods off**:
`0x3088bb1 -> 0x3088bd8 -> 0x3088bf7 -> 0x3088da5 -> 0x3088e46 -> 0x3088e6a`,
each hit twice (the handler runs twice per `UnlockAchievement` call). Notably
this skips clean over `0x3088cbb` and `0x3088d81` -- session 3's two guessed
"type dispatch" candidate sites -- confirming they were never reachable for
real achievement calls in the first place, not merely untested.

### The branch never showed up where expected

Repeating the identical sweep with mods **active** was inconsistent: the
very first attempt (immediately after a fresh mods-on relaunch) hit *zero*
of the 19 sites -- the virtual dispatch fired (entry breakpoint matched
`rdi==0x80001669`) but nothing downstream did, suggesting a very early
bailout. But every subsequent attempt, on fresh achievement ids in the same
mods-on session, reproduced the **exact same 12-hit path as mods off**,
call-for-call. Fine-grained single-instruction sweeps of the handler's first
~10 instructions (the `test r14,r14; je 0x3088d6e` guard right after entry)
showed non-null arguments and normal fall-through in every mods-on capture.
Live user confirmation nailed it down further: even a call that reproduced
the mods-off path exactly (all 12 hits) still did **not** produce a Steam
toast. Conclusion: whatever gates achievements is not a stable branch inside
this specific traced function, or is a branch the 19 tested call/jmp sites
don't cover (the handler continues past `0x3088e6a` without further
calls/jmps in either mod state, based on this sweep, so the divergence -- if
it's in this function at all -- happens in plain code between existing call
sites, or in a part of the function this technique can't see). Chasing it
further here stopped being worth it.

### Finding the real Steam call site directly, and pivoting

Rather than keep guessing inside Osiris internals, targeted the actual Steam
API entry point instead. `bin/libsteam_api.so` exports
`SteamAPI_ISteamUserStats_SetAchievement`, but disassembling it
(`objdump -d --start-address=... --stop-address=...`) shows it's just
`mov rax,[rdi]; jmp [rax+0x38]` -- a thin C-ABI shim over the real
`ISteamUserStats` vtable slot. `bg3`'s own PLT has zero relocations against
that exported symbol (confirmed via `readelf -r`): it's a normal C++
Steamworks SDK consumer that calls straight through the vtable, so a
breakpoint on the exported wrapper never fires for real game calls. Resolved
the vtable slot at runtime instead: called the exported accessor
`SteamAPI_SteamUserStats_v012()` live from gdb
(`print ((void*(*)())0xADDR)()`), read `*iface` for the vtable pointer, then
`*(vtable+7)` (slot `+0x38`) for the real `SetAchievement` target, and set a
breakpoint directly there.

With mods active: the resolved real `SetAchievement` target **never fires**,
for any achievement id, confirming (independent of the Osiris-side tracing
above) that the block is real and sits somewhere between Osiris dispatch and
this exact call. With mods inactive: it fires, and the backtrace's frame 0 is
`(anonymous namespace)::hooked_set_achievement` inside **bg3le's own
`libbg3le.so`** -- session 2 had already installed a diagnostic vtable hook
on this exact slot (`preload.cpp`, "Steam achievement diagnostics") to log
real calls for the mods-on/off A/B test. bg3le already had the interception
point this fix needed; it just hadn't been used to *act* on the block yet.

### The fix: force the call, don't chase the check

Since the exact internal branch never pinned down cleanly, and bg3le already
had (a) a working hook on the real `SetAchievement` vtable slot with the real
function pointer cached (`g_real_set_achievement`) and the live interface
pointer (`g_user_stats_iface`), and (b) a working generic DIV-call
interception point (`call_wrapper`/`g_real_call`) that sees every
`Osi.*` dispatch by function id *before* the internal per-function handler
runs (so it fires regardless of what that handler decides) -- the simplest
fix is to skip finding the check entirely: when the dispatched function id is
`0x80001669` (`UnlockAchievement`), pull the achievement name straight out of
the `COsiArgumentDesc` chain and call the real Steam functions directly,
whatever the engine's own (possibly-blocked) handler goes on to do.

`maybe_force_unlock_achievement(id, arg_desc)` (`preload.cpp`):
walks the argument chain using the engine's own accessors
(`COsiArgumentDesc::GetOpaqueType`/`GetAnyString`, already resolved
elsewhere in this file via `next<...>`) looking for the **STRING**-typed
node (type `4`) -- the achievement id -- as opposed to the CHARACTER
argument, which is a **GUIDSTRING** (type `5`) and so can't be confused with
it even though both are "stringy". Then calls `g_real_set_achievement`
followed by a newly-added `g_real_store_stats` (found the same way: exported
`SteamAPI_ISteamUserStats_StoreStats` disassembles to `jmp [rax+0x50]`, slot
`+0x50`/index 10 -- cached, not hooked, since nothing needs to intercept it).
`SetAchievement` alone only stages the change in Steam's local cache;
**`StoreStats` is what actually syncs it and fires the toast** -- without it,
the very first successful forced call only showed up in Steam *after the
game process exited* (Steam's own idle/exit-time flush), not live, which is
what led to finding this.

Two bugs found and fixed getting this wired up:

1. **Anonymous-namespace scope mismatch.** The pre-existing Steam-hook code
   (`g_real_set_achievement` and friends) lived in its own `namespace { ... }`
   block declared *after* `namespace bg3le { ... }` had already closed --
   i.e. a completely different anonymous namespace at global scope, not
   nested inside `bg3le`'s. A forward declaration of
   `maybe_force_unlock_achievement` placed inside `bg3le`'s anonymous
   namespace (to call it from `call_wrapper`) silently created a second,
   unrelated internal-linkage function with the same name -- compiles fine,
   two warnings (`internal linkage but not defined` /
   `unused function`), completely dead at runtime. Fixed by moving the
   shared globals and the real function body up into `bg3le`'s own anonymous
   namespace, ahead of `call_wrapper`; the Steam-hook functions later in the
   file still find them via ordinary outward name lookup.
2. **`osi::set_handlers` got the pre-wrap pointers.** `maybe_wrap_div_table`
   calls `osi::set_handlers(copy[1], copy[2])` (wiring up `osi.cpp`'s own
   `g_call`/`g_query`, used by `osi::invoke()` -- bg3le's Lua `Osi.*` bridge)
   *before* overwriting `copy[1]`/`copy[2]` with the wrapper. Story-script
   (`.osi`) driven calls go through the DIV table itself and would have hit
   the wrapper; **Lua-triggered `Osi.UnlockAchievement(...)` calls -- i.e.
   every test this session used -- went straight to the raw original
   handler and never touched the wrapper at all.** This is why the first
   working build produced zero `EnableAchievements` log lines despite
   `call_wrapper` being correctly installed. Fixed by moving the
   `osi::set_handlers` call to after the (possibly-wrapped) assignment in
   both the always-on and `BG3LE_WRAP_DIV=1` diagnostic paths, so both entry
   points -- story-script and Lua -- share the same wrapper.
3. Made the DIV-call wrapping **always active**, not gated behind
   `BG3LE_WRAP_DIV=1` (that flag now only controls the *diagnostic*
   verbose-logging variant, `call_wrapper`). A new lean
   `achievement_gate_wrapper` (no per-call logging) is installed
   unconditionally as the DIV call handler so the fix ships by default.

### Live validation

With mods active, freshly loaded save, calling
`Osi.UnlockAchievement("BG3_Quest34", host)` via the Lua debug console:

```
DIV Call SAW UnlockAchievement id            [temporary; removed after confirming]
EnableAchievements: forced SetAchievement("BG3_Quest34") -> true
EnableAchievements: forced StoreStats() -> true
```

...and the achievement toast appeared in Steam immediately, no game restart
or exit required. Repeated successfully across multiple achievement ids.

Because `achievement_gate_wrapper` is installed both as the DIV table's call
slot (what story-script-authored `.osi` rules dispatch through) and as
`osi::invoke()`'s handler (what Lua's `Osi.*` bridge dispatches through), a
**real, gameplay-triggered** achievement unlock (a compiled story rule
calling `UnlockAchievement` on quest completion, as the game does natively)
should hit the same gate -- this session only exercised the Lua-triggered
path directly, so confirming a genuine in-game quest completion still
produces a toast with mods active is the natural next real-world check, not
yet done.

### Open ends

- The engine's actual internal mod-check (`ls::ModuleSettings::IsModded`'s
  Linux equivalent) was never conclusively located. This fix works by
  bypassing it, not neutralizing it, so unlike bg3se's tidy two-branch patch
  there's no single byte-level "this is the check" answer here. Acceptable
  for the goal (achievements work with mods active) but worth flagging for
  anyone hunting the check itself later.
- The pre-existing `osi.cpp` crash when `Osi.UnlockAchievement` is called
  Lua-side on an **already-earned** achievement is untouched by this fix --
  `maybe_force_unlock_achievement` runs *before* `g_real_call`/the internal
  engine handler, which is still invoked afterward and can still crash on
  that specific input. Steam's own `SetAchievement`/`StoreStats` are
  documented idempotent, so the forced calls themselves are safe to repeat;
  the crash risk is entirely in the pre-existing internal-handler call this
  fix doesn't touch. Still parked, still a separate bug.
