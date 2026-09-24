# Writing static data

`Ext.StaticData.Get(uuid, type)` reads a resource, and mods edit them:
5eSpells' whole reason for existing is adding and removing spells, which
means editing `SpellList` resources. Until this, that silently did
nothing — the read handed back a snapshot, `Ext.Types.Unserialize` filled
in the copy, and the mod reported success every time.

## What a write needs

Three things, and two of them work.

**A field writer.** `Ext._Internal.SetField` already wrote fields on ECS
components through `bg3le_meta_resolve` plus `write_field`. A resource is
the same call with a different base, which is
`Ext._Internal.ObjectSetField`.

**The id the engine already holds.** A `SpellList`'s `Spells` is a
`HashSet<FixedString>`, and a FixedString compares by *id*: a second entry
with the same characters is a different string to everything that looks at
one. So a name has to resolve to the engine's own id, and only text the
engine has never seen may be interned.

That needed the text-to-id lookup to be usable, and it was not. Walking
every bucket of every sub-table costs eighty milliseconds a call, so it
was indexed — and then the index was wrong twice over:

- It was built once. The engine allocates buckets as it interns, so the
  table grows all session: 262,425 entries while a level was loading, over
  a million by the time it had. An index built at the first lookup could
  not find a string interned afterwards, and the intern path then made a
  duplicate for text the engine already had. It now rebuilds when the
  table's bucket count changes.
- Each bucket was read in one call, and `process_vm_readv` returns a
  partial count when it reaches a page it cannot read, so the parse
  stopped there and abandoned the rest of the bucket. Buckets are read in
  pieces now, into a cleared buffer, so an unreadable page costs one piece
  and leaves zeroes rather than truncating everything after it.

With that, `FixedStringIntern("Shout_BladeWard")` hands back the engine's
id and logs no new entry, which is the whole point.

**Replacing the set.** This is the part that does not work yet.

## What is left, and what not to do

An element of a hash set cannot be written in place: the table's hashes
would still point at the old key. The set has to be rebuilt, and bg3se's
`HashSet<T>` has exactly the code for it — `clear()`, then `insert()` per
key, which rehashes and grows through the engine's allocator, since
`bg3le_install_game_allocator` points bg3se's `GameAllocRaw` at the
engine's own `operator new`.

Calling those methods takes the game down, and the reason is worth
writing down because it is the trap in reusing a vendored header for
anything but reading. `HashSet` is
`{ StaticArray<int32> HashKeys; Array<int32> NextIds; Array<T> Keys; }`.
*Reading* a set only ever touches `Keys`, so a spell list reads perfectly
whether or not the two members in front of it are laid out the way bg3se
describes. Writing touches all three: `clear()` fills `HashKeys` end to
end and `insert()` pushes onto `NextIds`. Somewhere in that prefix this
build does not agree, and an invariant check that the sizes are consistent
with each other is not enough to catch it — they are, and the mutation
still faults.

So the mutation is off unless `BG3LE_SET_WRITES=1`, and everything around
it is finished: the caller resolves each name to the engine's id, the
thunk is instantiated for the field's exact type, and
`Ext.Types.Unserialize` routes a set view to it and refuses cleanly when
it is declined.

The way to finish it is the way the rest of this repo works, and not by
calling someone else's struct: read one live set's three members, check
them against the keys it demonstrably has — `NextIds.size()` should equal
`Keys.size()`, `HashKeys.size()` should be a prime of at least
`Keys.size() * 1.5`, and every `HashKeys` entry should be either negative
or a valid key index whose `NextIds` chain terminates — and then do the
rebuild with plain writes at the offsets that survive that. The algorithm
is the part worth copying from bg3se, and it is short:

    insert(key):  keyIdx = Keys.size(); Keys.push_back(key)
                  NextIds.push_back(-1)
                  desired = Keys.size() + (Keys.size() >> 1)
                  if HashKeys.size() >= desired: hash it in
                  else rebuild the table at the next prime >= desired

    hash it in:   bucket = id % HashKeys.size()
                  prev = HashKeys[bucket]
                  if prev < 0: prev = -2 - bucket
                  NextIds[keyIdx] = prev; HashKeys[bucket] = keyIdx

`HashMapHash` for a FixedString is the id itself, and the bucket count is
`GetNearestMultiHashMapPrime(desired)`. Rebuilding the whole set from the
target key list in one pass — allocate all three buffers with the
engine's `operator new`, fill them, write the three headers, free the old
ones — avoids incremental growth entirely and is a single data
transformation with nothing borrowed but the arithmetic.
