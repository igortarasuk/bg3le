# Re-enabling achievements with mods active

Goal: port bg3se's `EnableAchievements` option (Windows) to bg3le, so mods
don't silently block Steam achievements. **Status: shipped fix (the
DIV-dispatch bypass, PR #1, commit `c1187b9`) works and is live-validated.**

**Update 2026-09-23: solved without the bypass.** The mod check is the
per-module predicate at raw VMA `0x37675f0`; patching it to `mov eax,1; ret`
(bg3se's IsModded approach) was validated live with a gameplay unlock. Read
`ACHIEVEMENTS-NEXT.md` and `PREDICATE-ANALYSIS.md` first; the rest of this
file is the historical diary and several of its labels are wrong (see
"Why the search kept failing" in ACHIEVEMENTS-NEXT.md).

## How bg3se does it on Windows

bg3se patches two functions directly: `ls::ModuleSettings::IsModded` (forced
to always report "not modded") and `esv::SavegameManager::ThrowError` (its
warning branch NOP'd out). Both are simple, same-size single-branch byte
patches found by AOB pattern scanning, since neither function is exported
even on Windows.

## Why bg3le takes a different approach

The Linux binary has no exported symbols at all for `IsModded`,
`HasCustomMods`, `ModManager`, or `EoCServer` -- everything is inlined with
nothing to search for by name. Extensive static analysis (objdump
byte-pattern scanning, a Ghidra headless import/decompile pass) and live gdb
tracing of `UnlockAchievement`'s native handler never turned up a single,
stable branch that flips between mods-on and mods-off: traced execution
paths were identical in both states for calls that got far enough to
compare. The actual check is either not a static conditional anywhere in the
traced code, or lives somewhere this approach couldn't reach.

A follow-up round got Ghidra's decompiler genuinely working (it had silently
been failing the whole time due to a lost executable bit on its native
`decompile` binary, plus a 0x100000 image-base offset nobody had accounted
for) and used bg3se's own `BinaryMappings.xml` to pin down the *one* real
GUID Windows checks (`d65cf1b6-23a8-f0db-0a56-4b479a559748`, not six). That
led to a genuine vectorized (SSE) check comparing a module GUID against ~18
packed "official module" constants -- but two live experiments (a
breakpoint on its entry, then a hardware watchpoint on the GUID constants
themselves) both showed zero hits while actually reloading a save with mods
active. So that function, whatever it's for, is not consulted at savegame
load time, and isn't the real mod-gate either. The search for the literal
check remains open; see "Known limitations".

Rather than keep hunting for that exact check, the fix bypasses it: bg3le
hooks Osiris' generic function-dispatch mechanism specifically for
`UnlockAchievement` (Osiris function id `0x80001669`), and when that call
happens, calls the real `ISteamUserStats::SetAchievement` + `StoreStats`
directly -- regardless of what the game's own (possibly-blocked) internal
handler goes on to do. See `src/preload.cpp`,
`maybe_force_unlock_achievement` and the `ISteamUserStats` vtable hook above
`call_wrapper`.

Live-validated: with mods active, calling `Osi.UnlockAchievement(name,
character)` now produces an immediate real Steam achievement toast, no
restart required.

## Known limitations

- **Update, Session 6: the engine's actual internal mod-check WAS
  eventually located and live-confirmed** -- see "Session 6: FOUND IT"
  near the end of this file. It's `esv::gEocServer`'s module-list count at
  offset `0x268` being non-zero, checked inside the real registered
  Osiris native handler for `UnlockAchievement` (raw VMA `0x3767780`).
  This fix still ships as a DIV-dispatch bypass rather than a surgical
  patch of that branch (the bypass already works and is live-validated;
  switching to a `ThrowError`-style patch of the real branch is a
  possible, not-yet-done follow-up, noted in Session 6).
- A pre-existing, unrelated bg3le bug crashes when `Osi.UnlockAchievement` is
  called on an **already-earned** achievement (a `COsiArgumentDesc`
  marshalling bug in `src/osi.cpp`, root cause still unknown). This fix
  doesn't touch that code path, so the crash risk remains.
- **Update, Session 6**: real, gameplay-triggered unlocks (killing the
  Adamantine Golem without the smith's hammer, twice, achievement
  `BG3_Quest33` / "Важка доля") were tested live via the same hook, not
  just Lua-triggered calls -- confirmed to hit the same code path.
- **Update, Session 7: the surgical patch (NOP-ing the `0x268` gate branch)
  was implemented, and the gate itself does open correctly and stays
  open** -- but achievements *still* don't unlock through it. There's at
  least one more, not-yet-found check further down the call chain
  (`FUN_0398e8e0` / `FUN_03867800`). See "Session 7" below. **The bypass
  was removed from the working tree while chasing this and needs
  restoring** (or the deeper check needs finding) before this branch is
  usable again.

## Session: chasing the savegame manifest parser (still open)

Followed the "likely next step" above -- traced the savegame manifest itself
instead of more GUID xrefs. Fully reverse-engineered the `.lsv` save format
from scratch, directly from the game's own parsing code (not from public
LSLib docs):

- **LSPK v18 header** (40 bytes): magic `LSPK`, `Version` (u32), an 8-byte
  `FileListOffset`, a 4-byte `FileListSize`, a 2-byte field, a 16-byte
  MD5-like hash, then (read last, appearing at the very end of the header
  bytes) a 2-byte `NumParts`. Confirmed against a real save:
  `FileListOffset + FileListSize == file size` exactly.
- **File table**: LZ4-block-compressed (`lz4.block.decompress`, not a raw
  LZ4 frame -- no magic bytes). Decompresses to a flat array of classic
  LSLib `FileEntry` structs (272 bytes each: `Name[256]` +
  `OffsetInFile1(u32)` + `OffsetInFile2(u16)` + `ArchivePart(u8)` +
  `Flags(u8)` + `SizeOnDisk(u32)` + `UncompressedSize(u32)`).
- **Per-file compression**: `Flags` byte `0x23` decodes as method=3 (Zstd,
  not the LZ4 used for the file table itself) + a level nibble. Successfully
  decompressed `meta.lsf` with plain `zstandard.ZstdDecompressor`.

`meta.lsf` (an LSF binary, magic `LSOF`) contains the literal field names
`HasMods`, `HasMissingMods`, `HasUnofficialMods`, `Validity`, `IsHonourMode`,
`IsCrossplay`, `IsConsoleFriendly`, `IsSelected`, `Description`,
`TimeString`, `Version`, `PlayTimeString` -- i.e. real, confirmed engine
concepts, not a guess. Found via two independent static occurrences in the
binary: a runtime-built reflection/property list (getter thunks like
`mov al,[rdi+0x278]; ret` for `HasUnofficialMods`, `+0x260` for
`HasMissingMods`, `+0x248` for `HasMods`, all siblings of `Validity` at
`+0x218`) and a separate static `{namePtr, typeId}` LSF-schema table.

**Ruled out**: live breakpoints on all three getter thunks (safe -- they're
tiny static code addresses, not heap watchpoints) while opening the Load
Game screen showed `HasMods` firing ~138 times (once per listed save) with
backtraces landing squarely in NoesisGUI (`BindingExpression::GetValue` /
`DataTrigger::RegisterBindings` / `FrameworkElement::ApplyTemplate`).
`HasMissingMods`/`HasUnofficialMods` never fired for a save without those
problems. Conclusion: this whole property cluster is the Load-Game-list UI's
mod-badge display data (NoesisGUI XAML data binding), not the
achievement-gate check. A follow-up single-shot write-watchpoint on a live
instance's `+0x278` (armed only after catching a getter hit, meant to
auto-detach after one hit to avoid repeating the earlier hang) found no
write before being manually aborted -- the field is very likely
zero-initialized in bulk during object construction, which happens before
any getter call gives us a "this" pointer to watch, so this approach can't
observe it without also solving the "watch before construction" problem.

**Revised hypothesis for the actual gate**: the previous session's live
trace of `UnlockAchievement`'s native handler already found *no* branching
difference between mod states for paths that got far enough to compare (see
above). Combined with this session's UI-badge dead end, the likely
explanation is that the check isn't a per-call runtime branch inside one
handler function at all -- it's more likely a **registration-time swap**:
Osiris' native-function dispatch table may bind a different function
pointer for `UnlockAchievement` depending on mod state evaluated once
(story-VM boot / save load), rather than branching inside a single shared
implementation. That would exactly explain "identical traced paths once
inside a handler" (nothing left to differ once you're inside whichever
implementation got bound) and why bg3le's DIV-dispatch-level bypass (which
intercepts calls *before* whatever's registered runs) works regardless.
Searching for a direct xref to the literal `"UnlockAchievement"` string (one
occurrence, in `.rodata`) turned up zero references -- neither as a
disassembled code xref nor as a raw stored pointer anywhere in the file,
which is consistent with large parts of this binary still being
undisassembled by Ghidra (this whole investigation has been done with
`-noanalysis` plus manual per-region fixes, since a handful of Ghidra bugs
were already found and fixed this way -- see above). A full,
unrestricted `analyzeHeadless` pass (no `-noanalysis`) was kicked off to
make future xref searches reliable across the whole binary; it's slow
(the binary carries embedded exception-table/LSDA data that doesn't fully
line up with the ad-hoc function boundaries created during this
investigation, producing many non-fatal `LSDACallSiteTable` errors during
the pass).

Also checked: the binary references external DWARF debug info by build-id
(`c8da62b137caebd03b57c23b65ba2547745ce851`, via `.note.gnu.build-id`, found
by Ghidra's `ExternalDebugFileSectionProvider`). No matching `.debug` file
exists locally, and `debuginfod-find` isn't installed / wouldn't have it
anyway (proprietary game binary, not a distro package) -- dead end, but
worth ruling out since it would have made everything above unnecessary.

Next concrete step once the full analysis finishes: retry the
`"UnlockAchievement"` string xref search (and similarly for any Osiris
native-function registration table it can now find), since Ghidra should be
able to see code it previously never disassembled.

## Session: pivot to raw symtab/objdump, real lead found (open)

Two things dead-ended the "wait for Ghidra" plan and led to a much faster,
Ghidra-independent approach that found the best lead of the whole
investigation so far:

- The literal Osiris function id `0x80001669` (`UnlockAchievement`) is **not
  a compile-time constant anywhere in the binary** -- confirmed by a raw
  4-byte LE search over the whole 224MB file (zero hits). It's a per-story
  symbol id Osiris assigns when it loads the compiled `.story` data at
  runtime, not something baked into `bg3`. Chasing xrefs to it was always
  going to fail regardless of how thorough Ghidra's analysis gets. Dead end,
  now understood *why* it's a dead end (not just that it is one).
- `bg3` ships a full, unstripped `.symtab` (152,400 entries, real demangled
  C++ names) -- `nm bg3 | c++filt`, not just `--dyn-syms`. This had not been
  checked directly before (`readelf --dyn-syms` alone was used earlier and
  only shows imported/exported symbols). It also turns out `COsiris::*` is
  entirely `UND` in `bg3` -- the whole Osiris VM is implemented in a
  separate, much smaller `libOsiris.so` (1.8MB vs 224MB), loaded via
  `DT_NEEDED`. `libOsiris.so` itself only exports the generic story-VM
  plumbing (COsiSmartBuf, COsiArgumentDesc, generic COsiris:: methods) and
  contains **no** achievement/mod strings at all -- the actual native
  function implementations and any mod-check are registered by `bg3` calling
  into this library, not implemented inside it.
- Bulk `objdump -d` over the whole `.text` section (90MB, ~2-3 min per full
  pass) plus `grep` turned out to be dramatically faster and more reliable
  than fighting Ghidra's function-boundary/no-return bugs for this kind of
  "find who references address X" question, since it's a linear sweep that
  doesn't depend on correct function-boundary recovery at all.

**The lead**: `bg3`'s `.symtab` has real mangled names for
`esv::LoadProtocol::LoadModule`, `LoadSavegame`, `HandleModuleLoaded`,
`LoadModuleAndLevel` -- all take `ls::ModuleSettings` (the exact class
bg3se's Windows patch targets) as a parameter. Like bg3se found on Windows,
none of these have their own function symbol (fully inlined), but each
left behind a `.L__FUNCTION__.<mangled name>` string constant in `.rodata`
(compiler-generated, used by an internal assert/log helper), at a fixed,
known address:

- `LoadModule`: string at `0x191ff3b`
- `LoadSavegame`: string at `0x1a64423`
- `HandleModuleLoaded`: string at `0x1a2a297`
- `LoadModuleAndLevel`: string at `0x19a6cbf`

A raw `objdump -d` + grep for RIP-relative `lea` instructions referencing
these four exact addresses (bypassing the "stored pointer" search that
failed for `"UnlockAchievement"` -- these are found as *disassembled code*,
not a data xref) found all four, and two of them -- `LoadSavegame`
(referenced at `0x41e6f0e`) and `LoadModuleAndLevel` (referenced at
`0x41e7870`) -- are only ~0x962 bytes apart, i.e. **inlined into the same
enclosing function**, which starts at `0x41e6ee0`.

Manually disassembled that whole function (`0x41e6ee0` onward, no Ghidra
needed). It's a real per-module validation loop: for each module entry in
the save's dependency list, it computes what looks like an FNV/xxHash-style
64-bit hash (a repeated `imul`/`xor`/`shr $0x2f` mixing sequence appears
twice per module, over two different fields -- almost certainly hashing a
module UUID/name and a version string) and compares it against a loaded
module's matching hash. Critically, it maintains a **16-bit per-module
status flags word at struct offset `+0x140`**: reset to 0 at the start of
each module's processing (`movw $0x0,0x140(%rbx)`), then conditionally
OR'd with different bit patterns depending on the hash-comparison outcome:

- `orb $0x1,0x140(%rbx)` -- set unconditionally after a version-string parse
  step succeeds
- `orb $0x4,0x140(%rbx)` -- set specifically when a dependency hash
  comparison **fails** (the `test al,al; jne <skip>` guarding it takes the
  "matched" path over this bit, so this bit means "did NOT find a matching
  loaded module" -- i.e. **missing/unrecognized module**)
- `orw $0x102,0x140(%rbx)` / `orw $0x201,0x140(%rbx)` -- set in two other
  branches, not yet correlated to a specific condition

This is by far the most promising concrete candidate found this whole
investigation for "the actual per-save modded/missing-content computation",
because it's sitting directly inside the *real*, non-UI `LoadSavegame`/
`LoadModuleAndLevel` inlined bodies (not the NoesisGUI badge properties
ruled out earlier), keyed off real dependency-hash comparisons, not display
formatting.

**Not yet done / next steps**:
1. Find who calls `0x41e6ee0` (a background `objdump -d | grep "call.*
   41e6ee0"` pass over the whole `.text` section was launched to answer
   this) -- that caller almost certainly aggregates every module's `+0x140`
   word into the save-wide "is this save modded / does it have missing
   content" verdict, which is the actual answer this investigation has been
   chasing since session start.
2. Once the caller is found, trace forward from there to see whether that
   aggregate verdict is what later gates `UnlockAchievement` (either
   directly, or via the same `ModuleSettings`-shaped object bg3se patches on
   Windows) -- this would finally let this port replace the current
   DIV-dispatch bypass with an actual "disable the check" patch, bg3se-style.
3. Correlate the two not-yet-understood bit patterns (`0x102`, `0x201`)
   against plausible flag names once the aggregation site's logic is
   visible (candidates: has-missing-content vs has-unofficial-content vs
   version-mismatch, given `ls::ModuleSettings` conceptually distinguishes
   these on Windows per bg3se).
4. The full unrestricted `analyzeHeadless` Ghidra pass (PID 457713 at last
   check, still running, ~50+ min elapsed) is left running in the
   background in case it's useful later, but is no longer the critical
   path -- raw `objdump -d` + targeted `grep` proved both faster and
   sufficient for this specific lead.

## Session continued: found the exact GUID-list comparison (still open)

Followed up on all four "next steps" above within the same session, all via
`objdump -d` + `grep`, no Ghidra needed:

- **Confirmed `+0x140` is read as well as written**, in the exact shape
  expected for a per-module gate: two call sites (`0x3c413c7` inside a large
  `esv::LoadProtocol`-shaped state-machine function, and an identical
  pattern at `0x40165e8` inside the *actual* `esv::LoadProtocol::DoState()`
  -- confirmed by nearby `.L__FUNCTION__` string refs to
  `CheckSwapServer`/`CheckStartLoadReady` at `0x40167d0`/`0x4016860`) both
  do: `cmpl $0xf, 0x18(%reg)` (state enum == 0xf, "process next module"),
  then `movzwl 0x140(%reg),%tmp; and $0x7,%tmp; jne <skip>` -- i.e. only
  reprocess a module if its low 3 status bits are still all clear -- then
  call the per-module handler at `0x41e6ee0` (the same function `LoadSavegame`
  and `LoadModuleAndLevel` are inlined into). This nails down `+0x140` as
  `esv::LoadProtocol`'s real, load-time-computed per-module validity
  bitfield, not a UI artifact.
- **Found the exact bit-computation site**, at the tail of the `0x41e6ee0`
  function (around `0x41e7c16`-`0x41e7c48`, part of the `LoadModuleAndLevel`
  inlined body): a call to a tiny helper at `0x4015060` returns a bool in
  `al`; that bool (inverted) becomes bit `0x2` of `+0x140` via
  `lea (%rcx,%rax,2),%eax` (OR-ing the bool into bit position 1), and
  additionally, if the *original* (non-inverted) bool was false, bit `0x4`
  also gets OR'd in. The exact same call target (`0x4015060`) is *also*
  called from deeper inside the `LoadSavegame` half of the same function
  (`0x41e750d`, guarding a `snprintf`-based warning-message path) -- i.e.
  this one helper is the shared "is this ok?" primitive both inlined
  functions rely on.
- **Decompiled `0x4015060` by hand** (it's a tiny, self-contained,
  prologue-less leaf function, trivial to read directly in
  disassembly, no Ghidra needed): signature `bool f(void* arrA /*rdi*/,
  int count /*esi*/, void* arrB /*rdx*/, int countCheck /*ecx*/)`. Logic:
  return false immediately if `count != countCheck`; return true
  immediately if `count == 0`; otherwise loop over `count` elements with a
  96-byte (`0x60`) stride in *both* arrays, and for each element do a
  16-byte SSE compare (`movdqu` + `pxor` + `ptest`) of the bytes at
  `elem+8` -- i.e. **compares two lists of 16-byte values (GUIDs) for exact,
  order-sensitive, count-matching equality**. This is a completely
  different, more central check than the vectorized ~18-constant
  "official module GUID" comparison ruled out earlier in this
  investigation (that one compared against a fixed list of engine-known
  GUIDs; this one takes *both* arrays as live parameters -- almost
  certainly "does the save's stored module-dependency GUID list exactly
  equal the currently-loaded module GUID list").

**Working conclusion**: this `0x4015060` array-equality check is the real,
low-level primitive behind whatever `ls::ModuleSettings`-level concept
(`Modded` / `HasMissingMods` / `HasUnofficialMods`) bg3se's Windows
`IsModded` patch reads on Windows. A mismatch here (save expects a module
that isn't loaded, or an extra module is loaded that the save didn't have)
is what sets bit `0x4` (and drives bit `0x2`) in the per-module `+0x140`
status word during `LoadSavegame`/`LoadModuleAndLevel`.

**Still not found**: the final aggregation step that ORs/ANDs together every
module's `+0x140` word into one save-wide boolean, and where *that*
aggregate gets consulted to gate `UnlockAchievement` specifically. The
`+0x140` word lives on a per-module entry (confirmed by the loop structure
walking an array of these in `0x41e6ee0`'s body), so there must be a
summarizing pass afterward (or a check of "is any module's `+0x140`
non-clean" scattered at the point achievements are unlocked/blocked) --
that pass hasn't been located yet.

**Important caveat found on the follow-up pass**: searched the *whole*
`.text` section for every caller of `0x4015060` (not just the two already
known) and found two more: `0x4014fe0` (a thin wrapper right next to
`0x4015060` itself) and, most informatively, `0x6efd908` -- which sits
inside a *third*, even cleaner instance of the exact same
`cmpl $0xf,0x18(%rdi)` / `movzwl 0x140(%rdi)` / `and $0x7` pattern, this
time directly identifiable as `esv::LoadProtocol::LoadModule` itself (its
`.L__FUNCTION__` string sits immediately after the call). Reading the
literal debug-log strings this function formats when the GUID-list check
fails (`"* ServerLoadProtocol: Load module request: %s"`,
`"* ServerLoadProtocol: Sending add-on load fail"`, and siblings like
`"...Received module loaded (%s) from %d %s"`, `"...Queueing module loaded
event for peer %d"`) makes clear **this whole `esv::LoadProtocol` machinery
is the multiplayer peer/module *synchronization* protocol** (validating
that a joining client's module list matches the host's), not necessarily
the single-player "was this save played with mods" check that gates
achievements. `0x6efd3f0` (the second helper contributing to the `+0x140`
bits, called with a forced/one-shot flag) turned out to be a generic
"log this warning once" utility (checks a `-1` sentinel field, formats and
hashes the message, returns whether it logged), not something
achievement-specific either.

So: the `0x4015060` GUID-list-equality primitive and the `+0x140` per-entry
status word are **real and precisely understood**, but their *confirmed*
role so far is multiplayer module-sync validation inside
`esv::LoadProtocol`, which reuses the same `ls::ModuleSettings`-shaped data
bg3se's Windows patch targets. Two possibilities going forward, both worth
checking rather than assuming either:
1. The single-player achievement-relevant `IsModded`-equivalent check
   consults this *same* underlying `ls::ModuleSettings` state (just via a
   different, not-yet-found caller/method, since `ModuleSettings` is a
   general "list of active content" concept usable from both single-player
   load and multiplayer sync) -- in which case the real target is a
   `ls::ModuleSettings::` method that reads a save-wide summary of this
   state, not `esv::LoadProtocol` itself.
2. Single-player mod detection is a genuinely separate code path that
   doesn't go through `esv::LoadProtocol` at all (plausible, since a
   solo game doesn't need peer-sync), in which case this whole thread,
   while now well-understood, is not the droid we're looking for, and the
   next lead should come from `esv::SavegameManager` (the *other* Windows
   patch target, `ThrowError`) instead -- `nm bg3 | c++filt` found **zero**
   symtab or `__FUNCTION__` hits for `SavegameManager` or `ThrowError`
   anywhere (fully inlined, no left-over string label at all, unlike
   `LoadProtocol`'s methods), so that side needs the AOB/pattern-scanning
   approach bg3se itself used on Windows, adapted for Linux -- e.g. finding
   `esv::SavegameManager`'s vtable (it should have RTTI -- `nm` /
   `readelf --dyn-syms` for `_ZTVN3esv15SavegameManagerE`-shaped typeinfo/
   vtable symbols, unlike plain member functions, often do survive even
   when methods are inlined) and working outward from confirmed vtable
   entries instead of hunting for the inlined method bodies directly.

Next concrete step: check whether `esv::SavegameManager` has surviving
RTTI/vtable symbols (`nm bg3 | c++filt | grep -i "SavegameManager"` already
came back empty for pure name matches, but `typeinfo for`/`vtable for`
entries mangle differently -- e.g. `_ZTVN3esv...` / `_ZTIN3esv...` -- worth
a dedicated grep pass before concluding there's nothing left of it either).

## Correction (same session): the LoadProtocol path DOES fire solo -- verified live

The "multiplayer-only" conclusion above was reached from a live gdb test
that came back with zero breakpoint hits -- but that test's negative result
was invalid: the save was never actually loaded during that run (confirmed
by the user afterward). Re-ran the exact same live test (via the new
`tools/bg3-attach.sh` wrapper -- see below) with the save genuinely loaded
this time, and got real hits:

```
[check] HIT caller1_state_check (#1) pc=<...+0x3c413ba>
  #2 <...+0x3c4143f>   (return addr right after `call 41e6ee0`)
[check] HIT GUID_list_equality_primitive (#1) pc=<...+0x4015060>
  #2 <...+0x41e7512>   (return addr right after LoadSavegame's own
                         `call 4015060` -- the *LoadSavegame* branch
                         specifically, not LoadModuleAndLevel's)
  #3-6: <...+0x2b062ea>, <...+0x2b05479>, <...+0x2afc750>, <...+0x2afc524>
  #7: <...+0x770577a>
```

So: **the `esv::LoadProtocol` GUID-list validation genuinely runs during an
ordinary single-player save load** -- the earlier "this is multiplayer
peer-sync, not relevant to solo achievement gating" conclusion is
retracted. Frames #3-7 (outside `LoadProtocol` itself) are all indirect
(vtable) calls -- `call *rax` / `call *0x10(%rax)` -- consistent with a
generic network-message-dispatch loop. This actually makes complete sense
for BG3's architecture: even solo play runs a local client talking to a
local, in-process `esv` (server) instance over the same message-passing
path a real multiplayer peer would use -- so "peer" in those debug-log
strings means "the local player's own connection to their own local
server," not "a remote human." The GUID-list check and the `+0x140`
per-module status word are therefore back on the table as directly
relevant to single-player mod detection, not a dead end.

**Tooling improvement made along the way**: replaced the old
"ask the user to hardcode a PID into `sudo gdb -p <pid> ...`" pattern
(which goes stale every time the game restarts) with a permanent
`tools/bg3-attach.sh` wrapper that auto-detects the real game PID itself
(the `bg3` process with the largest RSS -- there's also a lighter
watchdog/reaper process with the same name) and passes it to the `.gdb`
script via a `BG3_PID` env var, which the script uses to `attach` itself
explicitly. The user-facing command is now permanently
`sudo ~/Dev/bg3-modding/tools/bg3-attach.sh <script.gdb>`, no PID ever
appears in it again. (Also fixed a real bug hit while building this: naive
`line.split()` on `/proc/PID/maps` breaks because this install's path
contains spaces -- `.../Baldurs Gate 3/bin/bg3` -- so the path field must
be reassembled with `split(None, 5)` keeping the last field intact.)

Next step: with solo-relevance now confirmed, trace *upward* from frames
#3-7 above to find what actually triggers this LoadProtocol processing
during a solo load (likely a local-loopback network message carrying the
save's module list), and -- more importantly -- find where the resulting
per-module `+0x140` status ultimately gets summarized into whatever
`ls::ModuleSettings`-level state the achievement gate (or bg3se's Windows
`IsModded`) actually reads, since `LoadProtocol` itself only computes and
locally consumes it (for its own state-machine re-processing decision),
and no aggregate read/consultation site has been found yet.

## Correction #2 (same session): could not reproduce the solo-fire result

Tried to nail down the exact bit values by capturing the real `+0x140`
before/after a real load, for a clean save vs. a modded save, via
`tools/capture_module_flags.gdb`/`capture_module_flags2.gdb` (breakpoints
on all 3 known entry points -- `caller1`, `caller2`, `LoadModule` -- plus
their known exit/write points, reading `*(this+0x140)` directly rather
than trusting registers across the call).

- **First run** (clean, unmodded save, same game session as the original
  hit): got a real, semantically consistent result --
  `+0x140` went from `0x3f8` (bits 3-9 set, bits 0-2 clear -- which is
  *why* the `and $0x7` reprocess-gate let it through) to `0x303` (bits 0,1
  newly set, bits 3-7 cleared, bits 8-9 preserved, and critically **bit
  `0x4` -- the suspected "module missing/mismatched" bit -- stayed clear**,
  exactly as expected for an unmodified save).
- **Second run** (reloading the *same* save a second time, same process):
  zero hits on any of the 3 paths. Plausible explanation at the time:
  the validation is a one-time-per-process-lifetime thing, not
  one-per-load.
- **Third run** (modded save, same process, loaded from main menu after
  the above): also zero hits.
- **Fourth run** (full game restart specifically to rule out the
  "one-time-per-process" theory -- gdb attached and all 6 breakpoints
  confirmed armed *before* ever touching "Load Game", then the modded
  save picked as the very first load of the fresh process): **still zero
  hits** on all 3 paths.

So the "one-time-per-process" explanation doesn't hold up either -- a
completely fresh process, with the modded save as its first-ever load,
never reached any of `caller1`/`caller2`/`LoadModule`'s state-machine
checks. Since these are plain code breakpoints (unconditional on the
`state==0xf` value they check -- they'd fire regardless of what state the
object is in, the check happens *after* the breakpoint address), this
means the enclosing functions themselves were never called at all during
an ordinary solo "Load Game" from the main menu, on this attempt.

**Update -- reproduced, and now confirmed as a dead end for a different
reason.** Further live testing (loading several different saves back to
back, in the same already-running process, without restarting) showed
`caller1` firing reliably whenever the *newly requested* save's module set
differs from whatever was last active in that process -- i.e. it's a
"switching saves" recheck, not a "was this specific load modded" check.
Concretely: it fired for an old (unmodded) save loaded second in a
session (`+0x140`: `0x3f8` -> `0x303`, same as the original capture), and
then fired *again* for a genuinely modded save loaded third in the same
session -- but with **the exact same resulting value, `0x303`, bit `0x4`
still clear**. So even when this code path definitely runs against a real
modded save, it does not distinguish it from an unmodded one. Working
conclusion: this GUID-list check is most likely validating the *base
game's own* module integrity (or something else common to both cases),
not third-party mod presence -- a genuine, now twice-confirmed dead end
for achievement-gate purposes, not an inconclusive one. RTTI/vtable search
for `ls::ModuleSettings`/`esv::SavegameManager` also came back completely
empty (zero hits in the full 152K-symbol symtab for either name in any
form), consistent with both being non-polymorphic/fully-inlined classes.

## New lead: bg3se's own BinaryMappings.xml has the literal Windows logic

Rather than keep guessing at Linux code, read bg3se's own AOB pattern
definitions directly (`bg3se/BG3Extender/GameHooks/BinaryMappings.xml`,
available locally at `~/Dev/bg3-modding/bg3se/` and vendored into
`bg3le/vendor/bg3se/`) -- these encode the *exact* byte-level Windows
implementation bg3se patches, which is ground truth for what the check
actually does, not a guess:

- **`ls::ModuleSettings::IsModded`** (line 333): compares `eax` (the
  result of an earlier vectorized official-module-GUID membership test --
  this is almost certainly the same "~18 packed official module GUID
  constants" SSE check a much earlier session found and live-tested as
  seemingly not consulted at load time) against a specific sentinel
  `dword_145E0D6FC`, then sets a byte to 1 (modded) or 0 (not modded)
  depending on the match. bg3se's patch replaces the "set to 1" branch
  with "set to 0" unconditionally -- i.e. it doesn't touch the check
  itself, it just hardwires the *result* to "not modded" regardless of
  what the check found. This directly implies the earlier session's
  "ruled out" conclusion about that SSE GUID-constant check deserves a
  second look with today's better tooling, since bg3se's own Windows
  source says that check's result *is* what `IsModded` is built on.
- **`esv::SavegameManager::ThrowError`** (line 347): loads
  `esv::gEocServer` (a global), adds `0x108` to get a pointer -- i.e.
  **`ls::ModuleSettings` lives at offset `0x108` inside the global
  `EoCServer` object** -- calls an unnamed helper with it, then, if a
  flag `bl` isn't already set, calls **`ls::ModuleSettings::HasCustomMods`**
  on that same pointer and uses its boolean result to decide whether to
  escalate the warning. This is a second, sibling method to `IsModded`
  living on the same class, and gives a completely different, much more
  promising anchor to search Linux for than anything tried so far: a
  **global pointer to an `EoCServer`-shaped object**, with `ModuleSettings`
  at a **known, fixed offset (`0x108`)** inside it.
- **`esv::SavegameManager` itself** (line 478) is found via a distinct AOB
  anchored on a call to `esv::SavegameManager::RegisterFactory`, next to a
  reference to a separate global `esv::gSavegameManager` and a constant
  `0x15`/`21` (`esv::SurfaceActionIndex`) -- a second, independent global
  worth searching for if the `EoCServer`+0x108 route doesn't pan out.

Next step: search the Linux binary for the equivalent of `esv::gEocServer`
(likely a `.bss`/`.data` global pointer or object, referenced from many
places -- e.g. `.text` code loading a fixed address then indexing `+0x108`
before a call) and `esv::gSavegameManager`, using the same
`objdump -d`+`grep` approach that worked for the LoadProtocol thread, now
armed with concrete offsets (`+0x108`) and a concrete sibling function
name (`HasCustomMods`) to search for as anchors, instead of guessing at
inlined-function boundaries blind.

## New, better lead: `ls::ugc::CacheModListSingletonComponent`

The blind `+0x108` search (1563 raw hits, narrowed to 61 by requiring a
nearby RIP-relative global load *and* a nearby `call`) turned out to be
mostly generic templated ECS query-setup boilerplate (the same
`imul $0xf8,...`/array-indexing shape repeats for dozens of unrelated
component types), so `+0x108` itself wasn't the useful part -- but one of
the 61 candidates' nearby global reference named something genuinely new
and on-topic: **`ls::ugc::CacheModListSingletonComponent`** (`ugc` = User
Generated Content), with a sibling
**`ls::ugc::PendingModListRequestSingletonComponent`** -- i.e. a real,
first-party ECS singleton component that is *literally* "the cached mod
list," with an async pending-request companion (implying the mod list is
fetched/resolved once, asynchronously, and cached).

Found via the component's `TypeId<...>::m_TypeIndex` static globals
(`0x7d5bbd0` for the checked context, `0x7d4bad8` unchecked,
`0x7d5bc00`/`0x7d5bce0` for two different ECS query-spec instantiations
that include it). A full-`.text` xref scan for direct references to these
four addresses came back with only **22 hits total** -- far more tractable
than anything searched so far -- clustering almost entirely in a tight
~13KB code region (`0x7536e00`-`0x753a000`), plus a couple of unrelated
call sites elsewhere (`0x50f2c6d`, `0x5125-something`) that are presumably
other systems merely *reading* the cached list (e.g. UI).

Disassembled the whole 13KB cluster by hand. It's mostly the generic,
compiler-generated ECS `WorldView`/query-setup boilerplate that's
identical in shape for *every* system regardless of which components it
queries (index-into-component-array via `imul $0x58,...`/`$0xf8`,
mask-building, entity-iteration scaffolding) -- i.e. finding the component
name got us to the right neighborhood, but the actual per-entity business
logic (whatever decides what "the cached mod list" contains, and whatever
marks an entry as first-party/official vs. user-generated) lives in the
*body* of whatever system-update function this boilerplate is scaffolding
for, which hasn't been isolated yet -- manually tracing further through
this much raw disassembly is where this approach is hitting diminishing
returns.

**This is the best next target for Ghidra's actual decompiler** (as
opposed to more manual `objdump` archaeology): the function starting at
`0x7538ef0` (real prologue: `push rbp; push r15; push r14; push r13;
push r12; push rbx; sub $0x108,%rsp`) is one of the two real functions in
this cluster and is a good candidate to decompile once the background
`analyzeHeadless` pass (still running, ~3h+ elapsed, currently on
"Resolving External Symbols" which is typically near the end of the
pipeline) finishes -- Ghidra's proper C-like decompilation with real
variable/type recovery should make the actual per-entity logic legible in
a way raw disassembly reading has stopped being productive for.

## Session finale: Ghidra's full analysis finished, LoadProtocol thread definitively closed

The `analyzeHeadless` pass finished successfully after **~3h16m**
(11,789s), and confirmed it had imported the binary's full, unstripped
`.symtab` as real symbol names (verified: `__cxa_begin_catch`,
`_ZN6Noesis14BaseCollection3AddEPNS_13BaseComponentE`, etc. all resolve to
real Ghidra symbols) -- 380,387 functions recognized, vs. only the
manually-patched handful from earlier in this investigation. Also
reconfirmed: `-process <file>` **without** `-noanalysis` re-triggers the
*entire* analysis pass from scratch even on an already-analyzed program
(cost us a false start -- always pass `-noanalysis` for post-script-only
runs against an already-analyzed project; also, a killed headless run can
leave a stale `bg3proj.lock`/`bg3proj.lock~` that must be deleted by hand
before the project can be reopened).

**Corrected the Ghidra address-conversion formula** (the one used all
last session and earlier this session, `ghidra_addr = raw_file_offset +
0x100000`, was subtly wrong for `.text`): calibrated directly against two
independent known landmarks (the `UnlockAchievement` string's raw file
offset vs. Ghidra's reported address for it, and `nm`'s address for
`__cxa_begin_catch` vs. Ghidra's address for the same symbol). Both agree
exactly: **`ghidra_addr = VMA + 0x100000`**, where VMA is the plain
virtual address as reported by `readelf`/`objdump`/`nm` (i.e. `sh_addr`-
based), not a file-offset-adjusted value. The earlier formula only
happened to work by coincidence for `.rodata`, where `sh_addr == sh_offset`
already.

With working addressing, decompiled every address of interest found this
session in one pass (`tools/ghidra_scripts/DecompileKeyFunctions.java`,
output in `reference/ghidra_key_functions_decompiled.txt`). Highlights:

- **`GUID_list_equality_primitive` (0x4015060) decompiles perfectly**,
  confirming the hand-disassembled read exactly: `bool f(a, countA, b,
  countB) { if (countA != countB) return false; if (countA == 0) return
  true; loop comparing 16-byte chunks at 0x60 stride, return false on
  first mismatch, true if all match; }` -- a plain, count-and-order-
  sensitive GUID-list equality check, nothing more subtle than that.
- **`esv::LoadProtocol::LoadModule` (0x6efd8b0) decompiles completely and
  cleanly** (not cut short by the no-return-analyzer bug this time) --
  and it's now beyond doubt what it does: if `state==0xf` and the
  per-entry status word's low 3 bits are clear, it calls the GUID-list
  check; **if the check fails (lists don't match), it unconditionally
  sets bit `0x4`** in the status word (`(flags & 0xfff9) + 4 +
  bVar4*2` -- bit 2 forced on, bit 1 set from a second helper's result),
  resets several other fields, and notifies a list of registered
  listeners with the formatted `"Load module request: %s"` message. This
  is now a fully closed, fully understood function -- exactly matches the
  hand-derived reading from earlier in this session, just independently
  re-derived and confirmed via a real decompiler instead of manual byte
  tracing.
- The **caller1 site is not a small helper but case 4 of a large (1900-
  byte), ~0x41-case `switch` statement** that is unmistakably a generic
  inbound network-message dispatcher for `esv::LoadProtocol` (dispatched
  by a numeric message-type read from a ring buffer at `param_1+0x10`).
  Case 4 is specifically the "ModuleLoaded" message type; case 3 handles
  a sibling ("LevelCreated"-shaped) message the same way via a different
  function (`FUN_03d451c0`). This conclusively confirms the "peer/message"
  framing from earlier -- this whole mechanism only runs when a
  `ModuleLoaded`-class message is *received*, whether that message
  originates from a real remote peer or (as separately confirmed live)
  the local client's own loopback connection to its own local server
  during ordinary solo play.
- `FUN_06ffd3f0` (the "log-once" helper) and the `0x7538ef0` ModListCache
  stub both decompile exactly as hand-analysis predicted: a generic
  "log this message once, guarded by a `-1` sentinel field" utility, and
  a generic ECS query-descriptor-construction stub (real per-entity logic
  lives in whatever consumes the resulting `WorldView`, not in this stub
  itself) respectively.

**Conclusion for this whole thread, now with decompiler-grade confidence
instead of hand-disassembly inference**: the `esv::LoadProtocol` GUID-list
check is a real, correctly-understood, and *correctly functioning*
consistency check (does the module list a `ModuleLoaded` network message
claims match what's already active), used identically for real remote
peers and the local client's loopback connection during solo play -- but
it is not what gates achievements. Confirmed live (see the two live
capture rounds above) that it does not flag a real modded save as
mismatched, because from the local server's point of view its own active
module list *is* consistent with what its own local client is telling it
-- there is no actual peer disagreement to detect here in single-player.
This whole thread is closed.

**What's genuinely still open, going into any future session**:
1. `ls::ModuleSettings::IsModded` / `HasCustomMods` and
   `esv::SavegameManager::ThrowError` themselves (the two things bg3se
   actually patches on Windows) remain completely unlocated on Linux --
   zero symtab hits in any form (plain name, vtable, typeinfo,
   `__FUNCTION__` label) even against the fully-analyzed 380K-function
   database. They're fully inlined with no trace at all, unlike
   `esv::LoadProtocol`'s methods (which at least left `__FUNCTION__`
   labels behind). The AOB/pattern-scanning approach bg3se itself uses on
   Windows (matching *instruction shapes*, not names) is the untried
   option that most directly parallels how bg3se solved this same problem
   on the other platform -- worth building a proper signature (from the
   Windows byte pattern in `BinaryMappings.xml`, translated to the
   equivalent x86-64 SysV calling convention/register allocation) rather
   than continuing to search by name or by guessing at call sites.
2. The `ls::ugc::CacheModListSingletonComponent` ECS singleton is real,
   named, and about mods specifically (`ugc` = user-generated content),
   but its actual field layout and the system that populates/reads it
   meaningfully (not just the query-setup boilerplate found so far)
   hasn't been located -- the 22 direct xrefs to its `TypeIndex` globals
   are now known and could be walked further outward (from the query
   stub to whatever calls it, then to whatever iterates its results) with
   Ghidra's now-working decompiler instead of raw disassembly.
3. The earlier-session vectorized SSE ~18-official-GUID-constant compare
   (mentioned in bg3se's own `IsModded` byte pattern as the literal thing
   its result is built on) deserves a second live-tracing attempt with
   this session's much-improved tooling (`tools/bg3-attach.sh` auto-PID
   wrapper, corrected Ghidra addressing) -- the original ruling-out of it
   used cruder tooling from an earlier session and might have had the
   same kind of bugs (stale PID, wrong breakpoint address, timing) this
   session found and fixed repeatedly elsewhere.

## Found a genuine, independent prior effort in this repo: `recovered-symbols`

Discovered (via `git log`) that a previous, separate session had already
built something better than anything improvised today:
`tools/recover_symbols.py` + `reference/recovered-symbols-4.8.400.7143220.txt`
(9,427 entries). Method: the binary keeps `.L__FUNCTION__`/
`.L__PRETTY_FUNCTION__` string constants (11,214 of them) with their local
labels carrying the full mangled name in `.symtab`; a function that loads
such a string is, by construction, the function it names; `.eh_frame_hdr`'s
binary-search table gives **372,531** reliable function boundaries
(independent of and more trustworthy than Ghidra's own heuristic function
finder) to attribute each string load to its enclosing function.
Cross-checked against this file:

- Confirms, via a completely independent method, that `SavegameManager`,
  `ModuleSettings::IsModded`, and `HasCustomMods` have **zero** recoverable
  name anywhere -- not just no `__FUNCTION__` label reachable by hand
  search, but none in an exhaustive, systematic sweep either. This is now
  about as certain as it can get without Larian's own build artifacts:
  these functions simply never execute an assert/log statement that
  embeds `__FUNCTION__`, so no name-based recovery method can find them,
  Linux-side. Byte-signature matching (bg3se's own Windows approach) is
  the only path left for these two specific targets.
- No `esv::SavegameManager`, no `ls::ugc::` namespace entries, no
  achievement-specific Steam wrapper method turned up in this set either.
  Did surface a `stm::SteamAPIManager`/`stm::SteamMatchMakingManager`/
  `stm::SteamSocketOverride` namespace (Steamworks integration), but only
  for lobby/matchmaking/P2P concerns, not stats/achievements.
- The `function_starts()` helper in this script (wrapping
  `.eh_frame_hdr` parsing) is a genuinely reusable tool going forward: it
  gives an exact, decompiler-independent function boundary for *any*
  address, which would have avoided several of this session's own
  wasted cycles fighting Ghidra's/manual guessing at function boundaries.

**One more concrete data point** found while re-testing the Windows
`IsModded` GUID (`d65cf1b6-23a8-f0db-0a56-4b479a559748`) on Linux: the
string exists once (`.rodata`, raw offset `0x1a5631f`) and has exactly one
code xref, at `0x40905ca`. But the surrounding code is not a standalone
"official module list" check -- it's one iteration of a long, uniform
loop (~30+ repetitions, `.str.721.111917` through at least
`.str.749.111945`) that does, for each of a long series of unrelated
string constants (this GUID among them, interleaved with what look like
ECS component/query registration strings): hash the string
(`call 0x2438520`), then store the hash into a slot indexed by a
`TypeId<Spec<...>>::m_TypeIndex` global (`call 0x242e570`). This reads
like generic static-initializer-order adjacency (unrelated subsystems'
init code ending up next to each other because their translation units
were adjacent at link time), not a purpose-built "is this GUID one of the
N official ones" comparison -- i.e. this specific code site is probably
*not* where bg3se's Windows `IsModded` vectorized check lives on Linux;
the real check is likely elsewhere, keyed off this same string constant
via a different (perhaps genuinely vectorized/SSE, still unfound) call
site. Not chased further this session -- flagged here so a future pass
doesn't have to rediscover this same string reference from scratch.

## Session 5: found `LoadAchievementsDisabled` by string, traced it to a dead
## end (it's telemetry, not the gate) -- but with a strong structural
## confirmation the Windows `ThrowError`/`gEocServer+0x108` shape exists here too

Ghidra's full analysis (from the prior session) is done and stable; four
new Ghidra headless post-scripts (`DecompileKeyFunctions.java`,
`DecompileThrowErrorChain.java`, `DecompileAchDisabledCallers.java`,
`FindAchDisabledCaller.java`, all in `tools/ghidra_scripts/`) now
decompile arbitrary addresses in ~10s each via `-noanalysis` -- this is
now a fast, repeatable loop, not an overnight operation. **Gotcha re-hit
and re-confirmed**: `FUN_xxxxxxxx`/`DAT_xxxxxxxx` names *printed by* a
prior Ghidra decompile are already Ghidra-space (`VMA + 0x100000`) --
feeding them through the shift a second time silently decompiles the
wrong function 0x100000 bytes away. Cost one wasted run this session;
watch for it in any future script that chains a decompile off a previous
one's output.

**New lead, from `strings` not disassembly**: `strings -a bg3 | grep -i
achievement` turns up `LoadAchievementsDisabled` (among network-message
and other generic achievement strings) -- a name never found via the
`.eh_frame_hdr`/symtab recovery techniques (it's data, not a symbol).
Single xref (found via a custom fast RIP-relative-lea brute force scanner
over just the `.text` section -- full-binary scan in ~7s vs. hours for a
full `objdump -d`), at VMA `0x3b1b5b4`, inside a giant `switch` on a
`ushort` status code (~300+ cases spanning several non-contiguous ranges
via nested jump tables) that maps the code to a display name. Computed
which jump-table slot resolves to that address to get the exact enum
value: **`case 0x90` (144) = `"LoadAchievementsDisabled"`** (confirmed
directly in the full decompile of the switch too -- see
`reference/ghidra_throwerror_chain.txt`, `FUN_03c1b340`). Neighboring
cases in the same enum: `0x8f LoadMissingAddon`, `0x91
LoadHonourModeWhilePlaying` -- i.e. this is a real, in-use
`SavegameManager`-style load-status enum (mixing progress markers like
`LoadLevel_XyzManager` with actual error/warning codes), not a
coincidental unrelated string.

Traced the call chain:
- `FUN_03c1b340` (VMA `0x3b1b340`) -- the switch itself. Given a status
  code, dedups it (skips if already reported this session), converts to
  name, then calls `FUN_031f76b0(<esv::GameAnalyticsSystem instance>,
  name)`. **This is a telemetry/analytics event sink, not a gate** -- it
  unconditionally reports whatever code it's given, by name, to
  `GameAnalyticsSystem`. It has exactly 2 direct callers in the whole
  binary.
- One of those 2 callers, `FUN_02d2cb20` (VMA `0x2c2cb20`), is far more
  interesting: `void FUN_02d2cb20(long param_1, ushort param_2 /*status
  code*/, int param_3 /*"is this status mod-sensitive"*/)`. When
  `param_3 != 0`, it checks a global flag `*(char*)(DAT_07e19d20+0x10)`
  and a per-object flag `*(char*)(param_1+0x80)`; if either (or
  `param_1+0x82`) is set, it fetches a live module-list snapshot via
  `FUN_02d29950(buf, DAT_07c8c7f8 + 0x108)`. **`DAT_07c8c7f8 + 0x108` is
  the exact same offset bg3se's Windows `BinaryMappings.xml` uses for
  `esv::gEocServer + 0x108` in `esv::SavegameManager::ThrowError`** --
  strong independent confirmation this is the Linux build of the same
  object layout, and that `FUN_02d2cb20` is structurally the Linux analog
  of `ThrowError`'s mod-check gate (its 16 real callers, found via Ghidra
  reference search, pass `param_3=1` specifically for
  `TemplateNotFound`/`CountMismatch`-class codes -- exactly the class of
  error bg3se's own comment says `ThrowError` exists to downgrade for
  modded saves).
- **However**: exhaustively found *all* direct call sites to
  `FUN_02d2cb20` in the whole binary via a raw byte scan for `E8` calls
  targeting its address (45 total, vs. only 16 Ghidra could resolve to a
  containing function -- the other 29 sit in functions Ghidra's analysis
  never bounded, likely more of the recurring no-return-callee bug) and
  decoded the literal status-code immediate at every one that used a
  fixed immediate. **None pass `0x90`/144.** So `LoadAchievementsDisabled`
  is either genuinely dead in this build (an enum value with no live call
  site) or reported via a register-computed/dynamic status code this scan
  can't distinguish from the others -- either way, this specific
  telemetry pathway is a dead end for finding the actual gate.

**Conclusion**: `LoadAchievementsDisabled` is the *analytics event name*
Larian's own telemetry uses to record "achievements were disabled this
session" -- informational, sent to `GameAnalyticsSystem`, not the code
that actually disables them. The real gate (the Linux equivalent of
`ls::ModuleSettings::IsModded`) is almost certainly a small, standalone
check living near wherever `ISteamUserStats::SetAchievement` is actually
called from -- i.e. near the DIV-dispatch/Osiris `UnlockAchievement`
handler PR #1 already bypasses -- not in this savegame-load
status/telemetry machinery. **Next step for a future session**: decompile
the internal (non-Osiris, non-DIV-dispatch) handler that PR #1's
`preload.cpp` currently calls *through* (the original engine
`UnlockAchievement` implementation it bypasses), and look for an early
return keyed off a mod-status flag right there, rather than continuing to
chase the load/save status enum.

## Session 6: FOUND IT -- the real internal mod-check, live-confirmed

Did exactly the "next step" above, and it worked. Full chain, from Osiris'
own dispatch table down to the actual gate:

1. **Resolved the real registered native handler for `id=0x80001669`
   directly out of live memory**, with zero guessing, by walking Osiris'
   own function-registry data structures. bg3le's log already prints the
   generic call/query dispatcher addresses at story-load time (`DIV wrap:
   active (call=0x... query=0x...)`); decompiling that dispatcher
   (`FUN_02cd4500`, raw VMA `0x2cd4500`, registered into `COsiris` via
   `TOsirisInitFunction` -- this lives inside `bg3` itself, not
   `libOsiris.so`) showed it: takes the function id, computes
   `idx = (id >> 3) & 0x1ffffff`, looks up `registry->table[idx]` for a
   registered-function descriptor object, marshals arguments into a new
   `0xd0`-byte context object, then invokes the descriptor's stored
   **pointer-to-member-function** (Itanium ABI `{ptr, adj}` encoding,
   virtual-or-direct) via a generic thunk at `FUN_03088b70`.
   - **Correction to an earlier misidentification this same session**: a
     pre-existing file `reference/ghidra_unlockachievement.txt` labeled
     `FUN_03188b90`/raw VMA `0x3088b90` (20 bytes *after* the real thunk
     at `0x3088b70`) as "the native handler". Live-breakpointed it first
     and got 380 hits for completely unrelated NPC/prop template GUIDs
     (Zhent dungeon characters, forge levers, VFX helpers) during a single
     golem kill -- proof it's a generic, unrelated, extremely hot entity/
     template-name resolver. That file's labeling was wrong; flagging it
     here so nobody trusts it again.
   - Resolved the *actual* pointer-to-member-function live via gdb by
     reading the registered descriptor object's fields directly (no
     breakpoints needed for this part): `entry+0x48` = raw `ptr` (odd bit
     signals virtual dispatch), `entry+0x40`/`+0x50` = `this`/adjustment.
     For `id=0x80001669` specifically, `ptr` was **even** (non-virtual --
     a direct function pointer), resolving straight to **raw VMA
     `0x3767780`**.
2. **Decompiled `0x3767780`** (`FUN_03867780` in Ghidra-space naming).
   Six lines:
   ```c
   undefined8 FUN_03867780(undefined8 param_1, long param_2)
   {
     char cVar1;
     cVar1 = FUN_03867580(DAT_07c8c7f8 + 0x268);   // DAT_07c8c7f8 == esv::gEocServer
     if (cVar1 == '\0') {
       FUN_0398e8e0(*(undefined8 *)(param_2 + 0x18));  // the real unlock path
     }
     return 0;
   }
   ```
   `DAT_07c8c7f8` is the same `gEocServer` global already confirmed twice
   before this session (bg3se's Windows `ThrowError` AOB uses
   `gEocServer + 0x108`; this session's earlier telemetry-reporter thread
   used the identical global at the identical `+0x108`). Here it's read at
   a **different** offset, `+0x268` -- a second, distinct field, structurally
   a module-list (count at `+0x14`, entry array at `+8`, `0x60`-byte
   stride -- the exact same per-module record shape identified all the way
   back in this file's very first session).
   `FUN_03867580(list)` returns `0` only when that list's count is `0`;
   otherwise (count > 0) it returns nonzero. So: **if `gEocServer`'s
   module list at `+0x268` is non-empty, the real unlock call
   (`FUN_0398e8e0`) is skipped entirely and the function just returns 0.**
   This is structurally identical to what bg3se's Windows patch defeats --
   a "has custom/active mods" check gating the achievement unlock -- just
   implemented as a plain count check here instead of a dedicated
   `IsModded()`/`HasCustomMods()` call.
3. **Live-confirmed with a breakpoint, twice, cleanly** (attached with
   `sudo gdb` directly this session -- see "sudo" note below -- no
   gameplay interruption, breakpoints auto-continue): with `BG3LE_NO_FORCE_ACH=1`
   (bg3le's own bypass disabled) and mods active, killed the Adamantine
   Golem without the smith's hammer (achievement "Важка доля" /
   `BG3_Quest33`, reset between attempts with the new
   `tools/reset_achievements.c` helper -- see below) twice. **Both times,
   all 4 dispatches of `Osi.UnlockAchievement` for this event hit
   `0x3767780`, and every single time `cVar1` (the `gEocServer+0x268`
   count check) came back nonzero -- the unlock path was never taken.**
   This is now about as confirmed as a non-source-code claim can be: live
   register values at the exact branch, on the exact registered handler,
   resolved via the engine's own dispatch tables rather than guessed.

**This closes the investigation's original goal.** The real Linux
equivalent of `ls::ModuleSettings::IsModded`/`HasCustomMods` is: *whether
`esv::gEocServer`'s module list at offset `0x268` is non-empty* (raw VMA
addressing; `DAT_07c8c7f8` in this Ghidra project maps to raw VMA
`0x7b8c7f8`). A future PR could replace bg3le's current DIV-dispatch
bypass with a proper bg3se-style patch: NOP the `test al,al` /
conditional-skip at `0x37677a1`-ish (right after the `call
FUN_03867580` in `FUN_03867780`) so the real `FUN_0398e8e0` unlock path
always runs -- mirroring exactly what bg3se does to `ThrowError` on
Windows, rather than bypassing the whole DIV dispatch. Not yet done this
session; the existing bypass already works and is shipped, so this would
be a "nicer, more surgical" follow-up, not a fix for a live bug.

**Tooling notes from this session, for future reference:**
- `tools/reset_achievements.c`: a standalone C tool (no Steamworks SDK
  needed, just `libsteam_api.so`'s flat exports) with `--list` (dumps
  every achievement's API name, earned state, localized name+desc --
  essential for mapping a display name like "Важка доля" to the opaque
  `BG3_QuestNN` API name Steam actually uses), `--clear NAME` (safe,
  single-achievement reset, used repeatedly this session to re-test one
  achievement without re-grinding), and `--reset-all` (wipes every
  achievement/stat -- deliberately gated behind an explicit flag, never
  run this session).
- `src/stackdump.cpp`/`.h` gained `dump_own_stack()`: the pre-existing
  `dump_all_thread_stacks()` explicitly skips the calling thread (it
  signals *other* threads and waits on them), so it was structurally
  incapable of ever showing who called into a hook on the current call
  path (e.g. who called `SetAchievement`). `dump_own_stack()` just calls
  `backtrace()` directly on the current thread -- no signal dance needed.
- `preload.cpp` gained `BG3LE_NO_FORCE_ACH=1`: a diagnostic-only env var
  that makes `maybe_force_unlock_achievement` a no-op, letting the
  engine's own (gated) handler run so its real behavior can be observed.
  Confirmed via this flag, live, that with mods active the engine's own
  handler never reaches `ISteamUserStats::SetAchievement` at all --
  necessary groundwork before finding the exact branch above.
- **Passwordless sudo for `gdb` only** was set up this session
  (`/etc/sudoers.d/`, `NOPASSWD: /usr/bin/gdb`) so live inspection no
  longer needs the user to run every gdb command by hand. Caveat hit
  twice: the rule matches the literal executed command, so wrapping gdb
  in `env` or `timeout` (e.g. `sudo timeout 90 gdb ...`) still prompts for
  a password, because `timeout`/`env` -- not `gdb` -- is what sudo sees.
  Fixed by calling `/usr/bin/gdb` directly and building any needed
  timeout *inside* the gdb Python script itself (a `threading.Timer`
  posting a `detach` via `gdb.post_event`) instead of wrapping the
  process. Also: ptrace semantics mean a killed/exited tracer
  auto-detaches its tracee, so even a hard failure here is safe -- the
  game was never at risk of hanging.
- Two Ghidra-address mixups cost time this session and are worth stating
  plainly for the next one: (1) a `FUN_xxxxxxxx`/`DAT_xxxxxxxx` name
  *printed by* a Ghidra decompile is already Ghidra-space (`raw VMA +
  0x100000`) -- feeding it through the `+0x100000` shift again silently
  decompiles/disassembles the wrong address entirely, off by exactly that
  amount. (2) Always double check *which* convention a given script/file
  used before reusing an address from it -- this file now has examples of
  both raw-VMA-input and Ghidra-space-input scripts, distinguished only by
  their own inline comments.

## Session 7: surgical patch implemented, gate confirmed open, but a
## deeper check still blocks the unlock -- investigation left open here

**Goal for this session**: replace the working DIV-dispatch bypass with a
proper byte patch of the branch found in Session 6 (NOP the `jne` right
after `call FUN_03867580` in `FUN_03867780`, raw VMA `0x37677a3`, 2 bytes
`75 43` -> `90 90`), mirroring bg3se's Windows `ThrowError` patch style.
**Result: the patch works exactly as designed (the gate opens and stays
open), but achievements still don't unlock -- there's at least one more
check further down the call chain that this session did not find.** The
bypass was removed from the working tree to make room for this attempt and
was not restored -- **the working tree is currently broken**, see the
warning at the top of this file.

### What was built

1. `src/hook.cpp`/`hook.h` gained two small primitives, both link-address +
   load-bias based like the existing `hook_slot`/`hook_call_sites`:
   - `patch_bytes(offset, expected, patch, len)`: mprotect RW, memcmp
     against `expected` first (refuses if it doesn't match, same safety
     net as `hook_slot`), memcpy `patch` in, mprotect back to RX. Logs
     success/failure.
   - `bytes_match(offset, expected, len)`: a read-only memcmp, no
     mprotect -- cheap enough to poll every server tick.
2. `src/preload.cpp` gained `ensure_achievement_gate_patch()`: checks
   `bytes_match` against the NOP bytes first (cheap, early-return if
   already patched), otherwise calls `patch_bytes` to (re)apply, logging
   distinctly for "first install" vs. "engine reverted it -- re-applied".
   Called from two places: right after
   `maybe_wrap_div_table(init_fn)` inside the
   `COsiris::RegisterDIVFunctions` interposition (every time Osiris
   (re)registers, which happens once at process start and again whenever
   a save loads), and once per server tick from `update_messages_hook`
   (a cheap safety net, in case something *other* than
   `RegisterDIVFunctions` ever turns out to cause a revert).
3. The entire old bypass was **removed**: `maybe_force_unlock_achievement`,
   `achievement_gate_wrapper`, and the always-installed wrapper in
   `maybe_wrap_div_table` are gone. `call_wrapper`/`query_wrapper` are now
   pure diagnostics (only active under `BG3LE_WRAP_DIV=1`); by default
   `maybe_wrap_div_table` just returns `init_fn` unmodified.
   `g_user_stats_iface`/`g_real_store_stats` (only needed for the forced
   calls) were deleted as dead code. The `ISteamUserStats::SetAchievement`
   vtable hook (`hooked_set_achievement`, logs+forwards, doesn't force
   anything) was kept -- it's the thing that would prove the real unlock
   path reached Steam.
4. Cosmetic but worth keeping: the `NoStoryLoaded` pump log line was
   confusing (reads like "no story is loaded", when it's actually just a
   poll-rate counter for a function the engine calls constantly regardless
   of story state) and cost real back-and-forth with the user before being
   understood correctly. Reworded to `"pump: NoStoryLoaded() poll rate ..."`.

### The critical process-topology discovery (cost most of this session)

**bg3 runs as two separate OS processes**, not one: `pgrep -x bg3` always
returns two PIDs. `cat /proc/<pid>/status | grep PPid` shows one is the
parent, the other's `PPid` equals the parent's PID (a real `fork()`, not
just threads -- separate `Pid`/`Tgid`). Both processes map the **same**
`bg3` executable at the **same** base address (checked via
`/proc/<pid>/maps`, the line ending `/bin/bg3`) -- consistent with the
fork happening after the initial image (and ASLR layout) was already
established, i.e. no re-exec in between.

**The parent's own `bg3le.log.<pid>` contains all the visible, "obviously
relevant" logging** -- `COsiris::RegisterDIVFunctions()`, `StoryLoaded()`,
`COsiris::Event(...)`, the Steam `SteamInternal_FindOrCreateUserInterface`
calls, the `ISteamUserStats` vtable-hook install line. This makes the
parent look like "the" process to debug. **It is not** -- for the
achievement path specifically, the code that actually executes
`esv::gEocServer`-touching Osiris native handlers (e.g. `FUN_03867780` at
raw VMA `0x3767780`) runs in the **child**, confirmed by attaching gdb to
*both* PIDs simultaneously with an identical entry breakpoint at
`0x3767780 + bias` and triggering several achievement attempts in-game:
**zero hits in the parent, eight hits in the child**, in the same time
window. The child never writes its own `bg3le.log.<its-pid>` (it inherited
bg3le's already-initialized state via `fork()`, so its `__attribute__((
constructor))` never re-runs and `log_init()` never opens a
child-specific file) -- so anything that needs observing on the child's
side has to be done live (gdb breakpoints, direct `/proc/<pid>/mem`
reads), never via its log.

**This resolves a false lead from earlier in this same session**: partway
through, a byte-patch that had logged as successfully applied
(`"EnableAchievements: patched internal mod-gate at 0x37677a3"`, written
to the *parent's* log) was later found, via direct `/proc/<pid>/mem`
reads, to show the original unpatched bytes again. This was interpreted at
the time as "the engine reverts the patch" and is *why*
`ensure_achievement_gate_patch()`'s watcher exists at all. **In hindsight
this was very likely a misdiagnosis**: the process being read
(the parent) simply never has a meaningfully-executed copy of that code
path to begin with, so whatever its bytes say doesn't actually matter --
the *real* (child-process) copy, once directly checked, was confirmed
**still correctly patched (`90 90`) and had NOT reverted**, even after
the same story-reload event that supposedly triggered the "revert" in the
parent. **The watcher is still worth keeping** (it's cheap and harmless,
and a real per-process revert was never fully ruled out over a long
session on the *correct* process either), but treat the original "the
engine actively fights the patch" theory as unconfirmed / probably wrong,
not as an established fact.

**Practical rule for future sessions**: when live-debugging anything
achievement/Osiris/`esv::`-related, always identify and attach to the
*child* PID (the one whose `PPid` matches the other bg3 PID), not
whichever one `pgrep` happens to list first. `tools/gdb_scripts/
entry_watch_gen.py` (new this session) generates a parameterized,
non-interrupting entry-breakpoint gdb script (attach, log rdi/rsi on
every hit, auto-detach after a timeout) -- used this session to watch
both PIDs at once and get the decisive answer; reuse it rather than
hand-writing another one-off script.

### Where it's stuck: the gate opens, but `SetAchievement` still never fires

With the gate patch confirmed live and open in the child process (`90 90`
at `0x37677a3`, entry breakpoint fired 8 times across a round of
achievement attempts), `ISteamUserStats::SetAchievement` was still **never
called** -- checked via the existing vtable-hook diagnostic (`hooked_set_
achievement`, which unconditionally logs every call and forwards to the
real function); its install line appears once in the parent's log
(`"steam hook: ISteamUserStats vtable ... SetAchievement slot -> ..."`),
but no `"ISteamUserStats::SetAchievement(...)"` call line ever followed,
across multiple achievement attempts. Cross-checked independently against
Steam's own state via `tools/reset_achievements --list`: `BG3_Quest33`
("Важка доля") stayed `earned=no` throughout.

Conclusion: **the `gEocServer+0x268` module-count check (Session 6) is not
the only gate.** Something further down the call chain --
`FUN_0398e8e0` (called directly inside `FUN_03867780` once the gate is
open) or `FUN_03867800` (called after that, does more work with `strlen`
and some kind of formatted-message construction, per the Session 6
decompile) -- still stops short of actually reaching
`ISteamUserStats::SetAchievement`. Neither of these two functions has been
decompiled beyond the one-line "the real unlock path" label from Session
6 -- that's the next concrete step: full Ghidra decompiles of both
(raw VMA `0x398e8e0` and `0x3767800`; remember `ghidra_addr = raw_VMA +
0x100000` for feeding them to a Ghidra script), tracing where they
actually branch on something mod/save-related, if anything. It's also
possible the real explanation is structural rather than a single new
branch -- e.g. the two processes each holding their own, independently-
initialized `ISteamUserStats*`/Steamworks connection, and only one of them
being the one actually wired to the real Steam client overlay -- worth
checking directly (read the vtable pointer + the patched slot's bytes in
*both* processes, the same way the gate bytes were cross-checked) before
assuming it's another code-level gate.

This is unresolved as of the end of this session. Options going into the
next session, in the order they were presented to (and left with) the
user: (1) restore the working DIV-dispatch bypass (fast, known-good,
reference `c1187b9`/PR #1/branch `achievements-pr`), or (2) keep chasing
this chain. The user chose to keep chasing it, but wanted this full
write-up committed to memory/reference first so a fresh session tomorrow
doesn't have to re-derive any of the above.

### Housekeeping

- No orphaned root-owned `gdb` processes were left running at the end of
  this session (checked with `ps aux | grep gdb` -- clean). If a future
  session sees leftover root-owned `gdb`/`sudo gdb` processes, they need
  `sudo kill -9 <pids>` (the `NOPASSWD` sudoers rule covers `gdb` itself,
  not `kill`) -- harmless to the game either way (ptrace auto-detaches a
  tracee when its tracer dies).
- The bg3 process was closed (exited normally, not crashed) by the end of
  this session; nothing to clean up there either.
- `reference/ghidra_*.txt` gained many new one-off dumps this session from
  now-abandoned side investigations (a `ls::ModuleSettings`/`HasCustom
  Mods`/`SavegameManager` thread that predates this specific session's
  work, plus some in this session chasing the `FUN_0398e8e0` lead that
  didn't get followed up). Not cleaned up; harmless clutter, left as-is
  per this project's existing convention of keeping raw decompile dumps
  around for reference rather than deleting them.
