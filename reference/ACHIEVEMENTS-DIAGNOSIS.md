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

## Next step

Same technique as `ecs_world.cpp`'s `EntityStorageContainer` capture: find a
stable instruction pattern that reaches `EoCServer` (or `ModManager`
directly) and hook/capture it once, the way the entity-storage lookup was
found by disassembling a known caller. Candidates to disassemble from:
`esv::LoadProtocol::LoadModule` (`0x6efd8b0`) definitely receives a
`ModuleSettings const&`, so tracing its caller chain backwards is one route
in; the "server tick" hook already installed (`preload.cpp`) is another
possible anchor if its argument turns out to be (or lead to) `EoCServer`.
