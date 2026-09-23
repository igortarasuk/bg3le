# Writing stats

`stat.Conditions = "..."` and `stat:SetRawAttribute(name, value)` are how
a mod changes the game's own stats at runtime. 5eSpells does it a few
hundred times in one `StatsLoaded` handler, and until this landed the
handler stopped at its first write.

## What an attribute is

One `int32` in the stat object's indexed properties, at the index its
modifier occupies in the object's modifier list. What the int32 *means*
depends on the modifier's kind:

| kind | meaning of the int32 | writable |
| --- | --- | --- |
| Int | the value | yes |
| Enumeration | the value; labels map to it | yes |
| Conditions | index into `RPGStats::Conditions` | yes |
| FixedString | index into `RPGStats::FixedStrings`, which holds string-table ids | see below |
| Float, GUID, Flags | index into the matching pool | no |
| StatsFunctors, RollConditions, Requirements | the engine holds these compiled | no |

So the write itself is a single store. What takes care is getting a value
into a pool first, because a value a mod builds at runtime is not in one.

## Adding to a pool

Growing the array is not an option: the engine owns it and frees it with
its own allocator, so replacing the buffer with one of ours hands it a
pointer to free that it did not allocate. What is available is the slack
past the end — a Larian array carries a capacity as well as a size, and
this build has plenty: the condition pool holds 5,620 of 8,192, the string
pool 30,989 of 40,000.

An entry goes into the spare capacity and **the size is raised to include
it**. The first version did not raise it, reasoning that an entry the
engine does not count is an entry it will never destruct. That was wrong
in the way that matters: an index past the size is out of range to every
reader, this file's included, so the attribute read back empty. Writing a
condition therefore *cleared* it — and an interrupt with no condition is
an interrupt that always fires. That is almost certainly what made the
engine grind during the first attempt at this.

Verified: after 5eSpells' pass, `Interrupt_AttackOfOpportunity.Conditions`
reads `"not S5E_IsInvisibleSeen() and IsAbleToReact(context.Observer)
and ..."` — the mod's prefix in front of the engine's own text.

## FixedString attributes do not work, and this is where it stands

A string attribute needs a string-table id, and text a mod builds at
runtime has none. `ls::FixedString` is a 32-bit index: sub-table in the
low four bits, bucket in the next sixteen, entry above that. The
sub-tables are length classes — entry sizes 48 through 2080, each holding
a 24-byte header and the text — and `field_1100` counts entries taken, so
the next entry the engine itself would hand out is the one at that index.

All of that works. `bg3le_fixed_string_intern` places an entry, the id
resolves to the text through the same path every other read uses, and the
pool slot holding it lands correctly:

    string table: interned 162 bytes as id 0xe500a66 in sub-table 6
                  (entry 48701 of 50224)
    stats: string id 0xe500a66 written to pool slot 30989

And then the engine spends the rest of the level load at 250% CPU in its
own code and never finishes. `perf` puts the time in `bg3`, not in
`libbg3le.so`, so it is the engine reacting to something, not bg3le being
slow.

Three theories tested and eliminated:

- **The entry counter.** Incrementing `field_1100` is what the engine does
  when it takes an entry, but taking one without incrementing it
  (`BG3LE_STRING_TABLE_BUMP=0`) stalls exactly the same.
- **The array size.** Raising the pool's size to include the new slot
  fixed the condition path completely, and changed nothing here.
- **The mod's own workload.** The handler never reaches its end, and the
  stall begins at the *first* string write, with conditions already
  written and logged before it.

What is left untested: the entry is not in the hash map the engine interns
through, and its `Hash` field is zero. Nothing should walk it — the map is
a separate structure — but that is the assumption this rests on, and it is
the next thing to check. The other candidate is the attribute itself:
`PotentSpellcasting.Boosts` is a passive's boost list, and the engine may
re-evaluate it in a way that a string it did not intern upsets.

`BG3LE_STAT_STRING_WRITES=1` turns it on for whoever picks this up.

## What is not attempted

`SetRawAttribute` on a functor attribute — `SpellProperties`,
`StatsFunctors` — needs the engine's own parser: those are held compiled,
not as text, and upstream delegates to `Object::SetRawAttribute`, which
has no symbol here. Storing an index to text the engine never compiles
would look like it worked and do nothing.

`stat:Sync()` reports rather than raising. The write already went to the
object the engine reads; what upstream's Sync additionally does, rebuilding
the spell and status prototypes, needs
`RPGStats::SyncWithPrototypeManager`, which also has no symbol. Raising
would be worse than saying so, because a mod that writes an attribute and
then syncs would lose the write it had already made.
