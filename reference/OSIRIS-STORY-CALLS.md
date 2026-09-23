# Calling the story's own Osiris functions

A mod calls `Osi.PROC_Foo(...)` or reads `Osi.DB_Bar:Get(...)` constantly,
and neither can go through the DIV boundary: the story's own functions
carry no dispatch handle. Osiris has 3,425 of them on a stock install and
none are reachable the way the engine's 983 are.

They run the way the engine runs them — a tuple is inserted into the Rete
node the function stands for — and bg3se does the same thing on Windows,
`node->InsertTuple(&tuple)`. What did not transfer is every number in
that sentence. This is where each one came from.

## The engine's own code was the source, not the layouts

`COsiris::Event` is exported. Raising an event *is* this operation, and
its disassembly does the whole job in order:

1. builds a `COsipParameterList` on the stack from its `COsiArgumentDesc`
2. finds the `OsiFunctionDef` by id in the function database
3. reads the node id out of the def, takes that node out of the node
   list, and calls **vtable `+0x68`** with the list
4. destroys the list

```
mov  0x8b0d3(%rip),%rax   # the node manager
dec  %r12d                # node id - 1
mov  0x8(%rax),%rax       # the element array
mov  (%rax,%r12,8),%rdi   # the node -- `this`
mov  (%rdi),%rax          # its vtable
lea  0x8(%rsp),%rsi       # the tuple
call *0x68(%rax)
```

bg3se puts `InsertTuple` at `+0x50`. Taking that offset would have called
`PushDownTupleDelete`. Two reasons it moves: the Itanium ABI spends two
vtable slots on destructors where MSVC spends one, and this Osiris has
virtuals bg3se's `NodeVMT` does not list — the non-query node classes
have 24 slots where bg3se describes 22.

Fitting bg3se's list against the slots *nearly* worked, which is the
trap. Slots 2 and 3 are both `mov 0x18(%rdi),%eax; ret`, matching
`GetDatabaseRef`/`GetDatabaseRef2` one slot along; slot 7 is
`mov (%rdi),%rax; jmp *0x30(%rax)`, which is `IsPartOfAProc` tail-calling
`IsProc` at slot 6, exactly bg3se's adjacency. Both anchors say "shift by
one". By `InsertTuple` the shift is three, and slot 11 — where a
one-slot shift puts it — is a bare `ret`.

## The structures

Read off the same two functions rather than ported:

    COsipParameterList  +0x00  vtable
                        +0x08  last node, and the sentinel's own address
                        +0x10  first node
                        +0x18  count
    node                +0x00  prev, +0x08 next, +0x10 TypedValue*
    TypedValue          +0x00  value, +0x08 type, +0x0a index, +0x0b flags

The `+0x08` field is itself the head sentinel: both pointers start out
holding its address, so the first insertion writes the first node through
what is nominally the sentinel's next pointer — which is the `+0x10`
field. `COsiris::Event`'s own walk confirms it by terminating on
`&list+0x08`.

`TypedValue` is bg3se's, to the byte: the engine initialises a fresh one
with `movl $0x2ff0000,0x8(%rbp)`, which is index `-1` and flags
`TypedValue`, and sets `IsValid` when a value goes in. bg3se's
`ValueFlags` and defaults match exactly.

The node classes are named, which is how a procedure node is told from a
database node without trusting a position in a list. Each vtable's
typeinfo is one slot behind it and carries the mangled name:

| vtable | class | nodes | bg3se's `NodeType` |
| --- | --- | --- | --- |
| `+0x10a960` | `CReteFact` | 8,111 | Database |
| `+0x10aa30` | `CReteEvent` | 9,060 | Proc, Event |
| `+0x10ab00` | `CReteDIVQuery` | 330 | DivQuery |
| `+0x10abe0` | `CReteOsiQuery` | 2,111 | UserQuery |
| `+0x10acc0` | `CReteInternalQuery` | 5 | InternalQuery |
| `+0x10ada0` | `CReteAnd` | 61,145 | And |
| `+0x10ae70` | `CReteNAnd` | 18,819 | NotAnd |
| `+0x10af40` | `CReteRelCondition` | 3,250 | RelOp |
| `+0x10b010` | `CReteRuleActionPart` | 51,032 | Rule |

bg3le reads that name at runtime and refuses to insert into anything that
is not a `CReteEvent` or a `CReteFact`, and checks that the slot holds
something other than a stub before calling it. A wrong slot number is the
one mistake here that takes the game down.

## Strings are interned

A `TypedValue` holding a string holds a handle, not a pointer: a fact
reads as `0x256d01df940f035` where a heap address on this build looks
like `0x52f40519ee0`.

`COsiStringTable::GetStr` is exported and its body is the entire
encoding, in eight instructions:

```
index = handle & 0x1fffff;        // the upper bits are never read
if (index == 0) return "";
return *(*(*this + 0x28) + index * 32);
```

So a record is 32 bytes with the text pointer first, the record array
hangs off the table at `+0x28` with its end at `+0x30`, and `this` is a
`COsiStringTable**` — bg3se types the export the same way. `AddStr`
agrees: it keeps a refcount at record `+0x18` and a free list of indices
at `+0x40`/`+0x48`. That record *is* bg3se's `COsiString`, `RefCount` at
`+0x18` and all.

An earlier search for that structure found nothing, and the reason is
worth keeping: it looked for one level of indirection where there are
two, and for the vector at the object's own start rather than at `+0x28`.
The layout was never wrong.

The table is the first of ten globals the `COsiris` constructor stores
(`+0x1116a0` here, 61,583 strings). bg3se finds the ten by a byte pattern
and indexes them in a fixed order; that order does not survive, because
this build embeds containers Windows heap-allocates — the node list at
`+0x119d40` and the database list at `+0x119ec0` are objects, not
pointers to objects. So the table is found by content instead, which
`GetStr`'s arithmetic makes cheap to test: dereference twice, check the
extent divides by 32, and check that every occupied record points at
printable text.

The upper bits of a handle are not a hash of the text and not an index —
they move in lockstep with the low 21 bits, a constant apart — and since
`GetStr` never reads them, nothing here has to reproduce them. An
argument is interned with `AddStr`, which is idempotent: asking for a
string the pool already holds returns the same index and one more
reference. Interned handles are released with `RemoveStr` after the call,
because the engine copies what it keeps.

One trap: the record array is a vector, so interning a *new* string can
reallocate it and move it. Caching its address and extent looked fine
until the first genuinely new string, which returned an index one past
the cached end and failed to read back. Every access re-reads both.

## Types decide whether a value is a string

Only some columns are. The story declares `CHARACTERGUID`; the engine
resolves that to `GuidString` through an array indexed by type id,
`{uint16 TypeId; uint16 AliasTypeId}`, walked until the id is `String` or
`GuidString`. Without it a type-1 integer value of `6` reads as the
string at index 6 and comes back as `"HealingSpiritHeal"`, which is how
the need for it was noticed.

That array is not in this process. A scan of every writable mapping for
sixty-four consecutive entries each holding their own index finds exactly
one match, an array of `{index, 1}` pairs, and it is something else.
Taking it made every aliased type an integer — so a GUID argument to a
procedure silently became `0`, and a fact written by the procedure's own
rule read back as `[0, ...]`.

It survived two rounds of checking because the obvious diagnostic cannot
see it. `resolve_alias(4)` and `resolve_alias(5)` return 4 and 5 whatever
the table says: they are base types and return before it is consulted.
The line printed "type 4 -> 4, 5 -> 5" and looked correct. Quoting a type
id above 5 would have shown it immediately, and the log does now.

What the story stores answers the question directly, with no structure at
all. A string handle's bits above the low 21 are never zero — consecutive
facts differ by `0x200001` — and a genuine small integer has nothing
there, so a column whose values all carry those bits *and* resolve to
text is a string, and one whose values are all under 2^21 is an integer.
The two do not overlap even for the first pool record. String or GUID
string is decided by the text, because `AddStr` wants to know which, and
a GUID string ends in a uuid.

On this save that learns 20 of them from 65,536 facts: 19 GUID strings,
one integer, one column with too little evidence. It is cached with the
signatures, because it belongs to the compiled story. An aliased type
that appears in no fact anywhere stays unknown, and then the value in
hand decides — reading goes by the shape of the value, and writing goes
by what the caller passed, which is strictly better than writing a zero.
A caller that passes a number where a known string type is declared gets
an error naming the type, as upstream raises for the same mistake.

## What it costs

Nothing, until a mod calls one. The `Function` objects behind these are
heap pointers that cannot be cached between runs, and recovering them
means walking Osiris' function database — half a second on the story
thread. So names resolve on first mention through a metatable, the way
bg3se resolves its own `Osi.*`, and a session where no mod calls a
procedure never pays. The level load spends 0.09s in bg3le either way.

## Retracting is +0x70

Found the same way, and worth spelling out because the neighbouring
slots suggest `+0x78`. The rule-action dispatcher branches on the
action's own insert/delete flag:

```
cmpb $0x0,0x10(%r13)
je   <add>                 ; logs " [add fact]",    calls *0x68
                           ; logs " [delete fact]", calls *0x70
```

Both arms take the node out of the same manager and pass the same
parameter list, and the engine names each operation itself in the string
it logs. Guessing from bg3se's spacing — `InsertTuple` then
`PushDownTuple` then `DeleteTuple`, eight bytes apart — would have picked
`+0x78`, which is the same mistake that puts `InsertTuple` at `+0x58`.

A nil column in a retract is a wildcard, which the engine wants as a
*cleared* value rather than an absent one: no type, `IsValid` off. That
is bg3se's `TypedValue::ClearValue`, and `LuaToOsi`'s `allowNil` does the
same thing on Windows.

Verified on `DB_CRIME_Assault_NoFallback`: one fact to start, two after
an insert, one after `Delete("BG3LE_DELETE_ME")` — the right one — and
none after `Delete(nil)`. The story's own fact was put back afterwards.

## How it was proved

`PROC_AnubisConfigs_DelayAssignment` has a database of the same name —
the standard Osiris delay pattern, where the procedure's rule inserts the
fact. Calling it with `(host, "BG3LE_PROBE_CONFIG")` left
`DB_AnubisConfigs_DelayAssignment` holding `[0, BG3LE_PROBE_CONFIG]`. The
tuple reached the node, the rule evaluated, and the procedure's body
wrote a fact derived from an argument bg3le interned. A direct
`Osi.DB_CRIME_Assault_NoFallback("BG3LE_PROBE")` shows the same for a
database node: one row before, two after, the new one reading back as its
text.

## Watching, rather than calling

`Ext.Osiris.RegisterListener(name, arity, event, handler)` with `before`,
`after`, `beforeDelete` or `afterDelete` is the same operation observed
instead of performed, so it is the same two slots: they are replaced in
the two tuple-holding node classes, and the replacement fires the
listeners and calls what was there. bg3se's `NodeHooks.cpp` patches
`InsertTuple` and `DeleteTuple` for its Database and Proc classes, which
are these two.

The vtables belong to libOsiris rather than to the executable, so there
is no link-time offset to check a slot against the way
`hook_slot` does. What stands in for that check is the class name behind
the vtable, read from its typeinfo: a slot is only patched in a vtable
whose class is `CReteEvent` or `CReteFact`.

Nothing is patched until a mod subscribes. Until then every node keeps
the engine's own pointers, and the reverse index from `Function` pointer
to name — which is what lets a firing node be named — is not built
either.

Engine-side activity reaches it, which is the point: a listener on
`DB_AnubisConfigs_DelayAssignment` fires for the fact the matching
procedure's *rule* inserts, not just for a fact bg3le inserts itself.
