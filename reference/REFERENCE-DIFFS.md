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

## `StatsExpressionPooled.Params` — measured, 2026-09-24

The first actual reading of it, so the next attempt starts from numbers
rather than from the shape of the declaration.

`Ext.Stats.Get("Target_MainHandAttack").SpellProperties[1].Functors[1]` is a
`DealDamage` whose `Damage` is a pooled expression. bg3le reads its `Code`
and `RefCount` correctly — `"Placeholder0"` and a live refcount, matching the
captured reference — and `Params` as:

    Params: one entry, the number 121

against upstream's

    "Params": ["Placeholder", 0]

Two things are wrong, and they are separate.

**The count.** `Array<Param>` is a pointer, a capacity and a size, and the
size is a `uint32` read through the container's own accessor — it does not
depend on `sizeof(Param)` at all. bg3le reads 1 where upstream reports 2. So
either the array being read is not the one upstream reads, or the field
offset of `Params` within `StatsExpressionInternal` is wrong here and the
size being read belongs to something else. The offset is the thing to check
first, and it is checkable: the buffer pointer next to it has to be a
readable allocation.

**The element.** 121 is `0x79`. `Param` is a nine-alternative variant, so its
discriminant is one byte in both libc++ and libstdc++, sitting after the
union — and the union's size is what differs between bg3le's build and the
engine's. Decoding as a number at all means the active index bg3le read
points at one of the integer alternatives (`int32_t`, or one of the four
enums) rather than at `Variant2`, which is where the string `"Placeholder"`
lives. 121 is not a byte of `"Placeholder"`, so it is not simply the string
read as an integer.

What this needs next is the raw bytes: the address of the `Params` field, its
buffer pointer, and a hexdump of the first hundred bytes of that buffer.
bg3le has `Ext._Internal.FieldAddress` for a component but no equivalent for
an object reached by address, so that is the small piece of tooling to add
first. With the bytes in hand the stride and the discriminant's offset are
read off rather than derived, the same way `HashSet`'s three members were.
