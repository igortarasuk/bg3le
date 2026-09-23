# Re-enabling achievements with mods active

Goal: port bg3se's `EnableAchievements` option (Windows) to bg3le, so mods
don't silently block Steam achievements. **Status: done, validated live on
2026-09-23.** Current design, verified facts and the test protocol are in
[ACHIEVEMENTS-NEXT.md](ACHIEVEMENTS-NEXT.md) and
[PREDICATE-ANALYSIS.md](PREDICATE-ANALYSIS.md).

## How bg3se does it on Windows

bg3se patches two functions directly: `ls::ModuleSettings::IsModded` (forced
to always report "not modded") and `esv::SavegameManager::ThrowError` (its
warning branch NOP'd out). Both are simple, same-size single-branch byte
patches found by AOB pattern scanning, since neither function is exported
even on Windows.

## What bg3le does

The Linux binary exports none of these names, but the same logic exists as a
small per-module "is this module official" predicate at raw VMA `0x37675f0`,
consumed by a "HasCustomMods" loop that the Osiris `UnlockAchievement`
native checks twice before sending the unlock to the client. Patching the
predicate to `mov eax,1; ret` at startup is the direct equivalent of bg3se's
`IsModded` patch; see `ensure_achievement_gate_patch` in `src/preload.cpp`.

## History

An earlier version (commit `c1187b9`) bypassed the gate by hooking Osiris'
dispatch for `UnlockAchievement` and calling `ISteamUserStats` directly. It
worked but was a bypass, not the check itself; the seven-session
investigation that led from it to the real predicate is kept as a diary
outside this repository.

## Known limitations

- A pre-existing, unrelated bg3le bug crashes when `Osi.UnlockAchievement` is
  called on an **already-earned** achievement (a `COsiArgumentDesc`
  marshalling bug in `src/osi.cpp`, root cause still unknown).
- Two inlined copies of the official-module compare (module-settings
  validation and a by-value variant) are deliberately left unpatched; they
  do not affect the unlock or the Load Game badges.
