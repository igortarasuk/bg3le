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

## FixedString attributes: correct, and still not usable

A string attribute needs a string-table id, and text a mod builds at
runtime has none. `ls::FixedString` is a 32-bit index: sub-table in the
low four bits, bucket in the next sixteen, entry above that. The
sub-tables are length classes -- entry sizes 48 through 2080, each holding
a 24-byte header and the text.

Free entries are kept on a list threaded through the headers'
`NextFreeIndex`, and **`field_1180` is its head**. That is not a guess:
every sub-table's `field_1180` has its own index in the low four bits --
0 through 10, all eleven of them -- which is the bottom of a FixedString
id, and the rest decodes the way ids do. For sub-table 6 the head reads
bucket 172 of 173 and entry 25 of 292, both in range, which they would
not be if the reading were wrong.

So an entry is taken the way the engine takes one: pop the head, put what
it pointed at in its place, with a compare-and-swap -- that 32-bit counter
above the id is the signature of a tagged lock-free stack, and the engine
interns on other threads throughout a level load.

Two wrong versions came first, and both are worth recording:

- **Taking the entry at `field_1100`.** That field is a count of live
  entries, not a high-water mark, so the entry sat in the middle of the
  allocated region and was *on* the free list. Writing over it cut the
  chain. The engine then handed the same entry out again -- a stat's
  `Boosts` read back as a Gustav animation path -- and spent the rest of
  the level load at 250% CPU, which is what walking a severed free list
  looks like from outside.
- **Popping with a plain load and store.** Correct with the game idle,
  and it lost the race every time during a load.

What works now, verified: an entry is placed off the free list, the id
resolves through the same path every other read uses, the pool slot lands,
and `PotentSpellcasting.Boosts` reads back as the mod's appended text
through a fresh `Ext.Stats.Get`. A staged probe during the level load
(`BG3LE_PROBE_INTERN=1/2/3`) shows each step is harmless on its own:
placing an entry, taking a pool slot, and pointing the attribute at it all
leave the load finishing in about a second.

What is not usable is a mod that does hundreds. With string writes on,
5eSpells gets past the write that used to stop it and then spends **369
seconds between one string write and the next**, in its own code -- the
console times out, so the story thread is inside the handler. It is
progressing, not deadlocked, and it never finishes in any reasonable time.

That is a performance problem in what the mod does after the point it
previously died at, and it is not the string writes themselves. Three
rounds of optimisation went in on the way and none of them was enough:

- modifier metadata, a list's modifiers and an object's indexed properties
  are each read once rather than once per attribute per stat;
- the decoded text behind a pool index is kept, since whole families of
  stats share the same expressions;
- `Ext.Stats.Get` no longer snapshots every attribute. It returns a proxy
  that reads on access, which is what upstream's does -- the snapshot cost
  two hundred decodes and two hundred table entries whether or not the
  caller wanted one of them, and because a mod keeps what it fetches, the
  collector's share of each fetch grew with the heap: 8ms per stat at two
  thousand, 40ms at six, 120ms at ten. Quadratic, and it read as a hang.
  Attribute names are indexed per modifier list so reading a field by name
  is a lookup rather than a walk.

Measuring the next step means instrumenting what the mod does in those six
minutes, not guessing again. `BG3LE_STAT_STRING_WRITES=1` turns it on for
whoever picks that up.

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
