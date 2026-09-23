# Re-enabling achievements with mods active

Goal: port bg3se's `EnableAchievements` option (Windows) to bg3le, so mods
don't silently block Steam achievements. **Status: done.**

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

- The engine's actual internal mod-check was never located, so this is a
  bypass, not a "this is the check, disabled" fix the way bg3se's is. Raw
  data from the search (a Ghidra xref dump, an offset-recovery tool) is left
  in `reference/ghidra_findings.txt` and `tools/offset_dump.cpp` for anyone
  who wants to pick that hunt back up.
- A pre-existing, unrelated bg3le bug crashes when `Osi.UnlockAchievement` is
  called on an **already-earned** achievement (a `COsiArgumentDesc`
  marshalling bug in `src/osi.cpp`, root cause still unknown). This fix
  doesn't touch that code path, so the crash risk remains.
- Only Lua-triggered calls were tested directly. A real, gameplay-triggered
  unlock (a compiled story-script rule calling `UnlockAchievement` on quest
  completion) goes through the same hook, so it should behave identically,
  but hasn't been separately confirmed live.
