# Re-enabling achievements with mods active

Goal: port bg3se's `EnableAchievements` config option (Windows) to bg3le.
Status: mechanism identified on the Windows side, live debug access confirmed
working end-to-end, native equivalent not yet located. Written up here so the
investigation survives a session boundary.

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
