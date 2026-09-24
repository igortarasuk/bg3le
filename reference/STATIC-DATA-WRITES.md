# Writing static data

`Ext.StaticData.Get(uuid, type)` reads a resource, and mods edit them:
5eSpells' whole reason for existing is adding and removing spells, which
means editing `SpellList` resources. Until this, that silently did
nothing — the read handed back a snapshot, `Ext.Types.Unserialize` filled
in the copy, and the mod reported success every time.

It works now. `Ext.Types.Unserialize(sd.Spells, arr)` — which is exactly
what 5eSpells calls — and `sd.Spells = arr` both replace the set, and a
fresh `Ext.StaticData.Get` reads back what was written.

## What a write needs

**A field writer.** `Ext._Internal.SetField` already wrote fields on ECS
components through `bg3le_meta_resolve` plus `write_field`. A resource is
the same call with a different base, which is
`Ext._Internal.ObjectSetField`.

**A snapshot that notices.** `Ext.StaticData.Get` returns a snapshot
rather than a live proxy, and filling the returned table directly made
every assignment a silent no-op: `__newindex` does not fire for a key the
table already holds. The values live in a table behind the metatable now,
so reading is still a snapshot and writing reaches the engine.

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

**Replacing the set**, below.

## Replacing a hash set

An element cannot be written in place: the table's hashes would still
point at the old key. The set has to be rebuilt, and bg3se's `HashSet<T>`
has exactly the code for it — `clear()`, then `insert()` per key.

Calling those methods took the game down, twice, and the reasons are the
useful part.

### Do not call the container's methods

`HashSet` is
`{ StaticArray<int32> HashKeys; Array<int32> NextIds; Array<T> Keys; }`.
*Reading* a set only ever touches `Keys`, so a spell list reads perfectly
whether or not the two members in front of it hold what they should.
Writing touches all three, and does two things bg3le must not:

- `clear()` fills the engine's existing `HashKeys` buffer in place. Static
  data comes out of a .pak; that the buffer is writable is not something
  to assume.
- `Array::clear()` hands the engine's buffer to `GameFree`, which is the
  engine's `operator delete`. bg3le did not allocate that buffer and
  cannot know what did.

So the rebuild allocates three fresh buffers, fills them, writes the
container's 48-byte header, and **abandons the old buffers rather than
freeing them**. Three small allocations per spell-list edit is a fair
price for never passing a foreign pointer to free.

### Do not call the container's hash either

`HashMapHash<FixedString>` is `FixedString::GetHash()`, which reads the
string table entry's hash *through an engine function pointer bg3le does
not have*. Calling it jumps to address zero — which is what the first
working version of the rebuild did, on the `ServerWorker` thread, with the
core naming `set_assign_thunk` two frames down from
`FixedStringBase::GetHash`.

bg3le reads that table itself, so the hash comes from there:
`bg3le_fixed_string_hash(id)` resolves the entry and returns its `Hash`
field. The id is **not** a substitute for it — a wrong bucket does not
fault, it makes the key unfindable, so a spell list would silently hold
nothing.

### Verified layout

Dumping a live `SpellList.Spells` with `Ext._Internal.SetDump` gave the
three members unambiguously — eleven keys in seventeen buckets:

    set dump: Spells at 0x2d10e5b8020, keys 0x2d167758200, 11 of them
      +00 = 0x000002d10e0c0c10   HashKeys.Buf
      +08 = 0x0000000000000011   HashKeys.Size   = 17
      +10 = 0x000002d167758300   NextIds.Buf
      +18 = 0x0000000b00000010   NextIds  cap 16, size 11
      +20 = 0x000002d167758200   Keys.Buf
      +28 = 0x0000000b00000010   Keys     cap 16, size 11

The thunk cross-checks those offsets against the compiler's own layout of
bg3se's `HashSet` on every call, so a header change refuses rather than
writing into the wrong member.

### Verified hash rule

The offsets can be read off a dump; the hash rule cannot, because a wrong
one is invisible until something looks a spell up. So it is checked
against the engine's own data before every write: `table_reproduces()`
walks each key the set already holds, computes its bucket the way bg3le
would, follows the `NextIds` chain from `HashKeys[bucket]`, and requires
that it arrive at that key — which is exactly what the engine's
`find_index` does. If the engine buckets by something else, the write is
refused and the log says so.

### The algorithm

bg3se's `ResizeHashMap` and `InsertToHashMap`, in one pass over the target
key list rather than incrementally:

    buckets = GetNearestSmallMultiHashMapPrime(count + 2)
    HashKeys[0 .. buckets) = -1
    for k in 0 .. count:
        Keys[k] = key
        bucket = hash_of(key) % buckets
        prev = HashKeys[bucket]
        if prev < 0: prev = -2 - bucket      -- how a chain ends
        NextIds[k] = prev
        HashKeys[bucket] = k

A negative `HashKeys` entry encodes the bucket it belongs to, which is how
a chain terminates without a sentinel. The bucket count comes from
`SmallMultiHashMapPrimes`, not `MultiHashMapPrimes` — the latter starts at
53, and a live eleven-key set had seventeen.

The header is written empty-first (`Keys.Size = 0`, then the buffers, then
`Keys.Size = count`), so a reader that catches it mid-write sees a set with
no keys rather than one whose count outruns its buffer.

## Writing strings

`Progression.PassivesAdded` is an `STDString`, and string fields were not
writable at all — which only surfaced once the snapshot started writing
through, because before that the assignment was discarded before it got
anywhere near a field writer.

Both string kinds work now:

- `FixedString`: the name resolves to the engine's id, or is interned, and
  the 32-bit id is stored. The old id's reference is not released — bg3le
  did not take it, and an over-count keeps a string alive where an
  under-count frees one out from under a reader.
- `LSString`: Larian's sixteen-byte string, through `LSStringBase::assign`
  so the inline and heap forms stay its business. The storage is zeroed
  first, which abandons any old heap buffer instead of freeing it, for the
  reason above.
