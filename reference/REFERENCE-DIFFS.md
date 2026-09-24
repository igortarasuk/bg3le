# bg3le against the real extender

`reference/*.txt` is output captured from the Script Extender on Windows.
`tools/check-reference.sh` runs the same 25 queries against a running bg3le
and reports how far apart the answers are. This is where that stood on
2026-09-24.

    25 queries: 12 identical, 2 differ only in order, 10 differ in content

Addresses are normalised before comparing — upstream's pointers are Windows
addresses and ours are this process's — and a difference that disappears when
both sides are sorted is reported as an ordering difference, because a dump's
key order is whatever `pairs()` gave and Lua does not define it.

## What the harness found

Three queries failed outright, and two of the three were real bugs:

- **`Ext.Entity.GetAllEntitiesWithComponent` returned handles**, where
  upstream returns entity objects. A caller writing
  `for _, e in ipairs(...) do if e.DisplayName` got "attempt to index a number
  value". `GetAllEntities` and `GetAllEntitiesWithUuid` had it too.
- **`entity:GetAllComponents()` did not exist.** It does now: bg3le asks the
  question the other way round from upstream — for each component it has
  metadata for, does this entity carry it — which reaches the same set, since
  a component bg3le cannot describe could not be returned either way.
- **`AttributesUnavailable` appeared in every stat dump** as a null. It is
  bg3le's own diagnostic, not one of upstream's keys, and a mod iterating a
  stat would have seen it. It is listed as a diagnostic now and appears only
  when it has something to report.

After those, `entity-component-health` is byte-identical to the real
extender's capture, and `stats-weapon` and `stats-base-weapon` match in
content with only their key order differing.

## What still differs, and why

**The install is not the same one.** Eight of the ten are this and nothing
else: `stats-count` (24k stats against upstream's), `mod-loadorder` (a
different mod set), `entity-host` and `entity-component-list` (a different
save), `staticdata-actionresource` (125 action resources against 87),
`entity-component-types` and `types-count` (a newer bg3se's metadata),
`enums-list` (296 enums against 295 — bg3le has `SurfaceTransformActionType`
and the capture does not).

**Two are real representation differences**, both in the functor and
expression area that `reference/STAT-WRITES.md` already describes as partial:

- A dice expression reads as its source text where upstream decodes it into
  fields. Upstream's `BURNING` has `AmountOfDices: 1`, `DiceValue: "D4"`,
  `DiceAdditionalValue: 0`, `DiceNegative: false`; bg3le has `Code: "1d4"`.
- Upstream's dumper prints `*RECURSION*` where a nested functor refers back to
  its parent. bg3le's objects are built fresh per read rather than being the
  same proxy, so the cycle is not there to detect and the structure expands.

**One is cosmetic.** Upstream's static data entries carry a `Get` method that
shows up in a dump; bg3le's do not.

## Using it

    ./tools/check-reference.sh          # needs the game running with bg3le

The queries are parsed out of `tools/grab-reference.sh` rather than restated,
so the capture script and the comparison cannot drift. bg3le's answers land in
`reference/bg3le/` for diffing.

It deliberately does not decide pass or fail. Most of the remaining
differences are the install, and a harness that called those failures would be
ignored within a week. The number worth watching is "differ in content" on the
queries that do not depend on the save or the mod set.
