# Native Linux build: 65-78s stall at 75% on every level load

Reproduces on a stock native build with no extender, no mods, outside the
Steam runtime container, on a brand-new game as well as an existing save.
The Windows build under Proton loads the same save in ~15s while running
BG3SE and 57 mods.

## Cause

Seven threads -- all six `WT/Low` workers plus `ServerWorker` -- occupy one
call path for the duration of the stall:

    physx::Sn::ConvX::convert(PxInputStream&, ...)      bg3+0x565fb70
    physx::Sn::ConvX::convertClass(...)                 bg3+0x5660120
    physx::Sn::ConvX::convert(void const*, int)         bg3+0x5661840
      -> checkCompatibility, getBinaryPlatformName, getBinaryVersionGuid,
         getMetaClass, _enumerateFields,
         TempAllocator::allocate / deallocate

`physx::Sn::ConvX` is PhysX's serialized-binary *platform converter*. Its
presence in the hot path means the cooked collision data being loaded is not
in this platform's binary format and is converted per asset at load time.

The threads are blocked rather than computing: they queue on PhysX's shared
`TempAllocator`, serialising work that is nominally parallel.

## Why the obvious measurements mislead

- **~10% CPU** while "busy": the threads are waiting on a lock. The 450%
  seen from `/proc` is the uncapped loading screen redrawing, not load work.
- **No disk I/O**: the source data is already in page cache.
- **Duration independent of level size** (a new game on the Nautiloid stalls
  the same) but varying 65-78s: lock contention, not a fixed timeout.
- **`sched_yield` backoff makes no difference** (2.3M yields, no change):
  the threads are blocked on a mutex, not starved of CPU.
- **~11M `clock_gettime`/sec**: the loading screen's frame pacing, unrelated.

## Ruled out

Cold caches (20+ loads, no I/O), the script extender (control run without
it), pressure-vessel (identical bare on the host), clocksource (`tsc`),
crossplay (`libParty`/`PlayFabPartyWrapper` never mapped in), Steam's
Fossilize and overlay Vulkan layers (disabled, no change), and network
timeouts (only a CloudFront connection, idle mid-stall).

## Also affected

A smaller stall before the main menu, which also loads physics assets.

## Method

Stacks captured in-process: `ptrace_scope=1` blocks gdb from attaching and
`eu-stack` cannot unwind past libc, so the extender signals each thread and
has it record its own backtrace (`src/stackdump.cpp`). Frames are logged as
link-time offsets and resolved against `.eh_frame_hdr` function boundaries
plus recovered symbol names (`tools/recover_symbols.py`). Unnamed functions
are identified by their callees (`tools/callees.py` approach).

Raw stacks: `stall-diagnosis-stacks.txt`.
