# Native Linux build: 65-98s stall at 75% on every level load

**Fixed.** Replacing PhysX's `TempAllocator` with a lock-free thread-local
pool takes the level load from 65-98s to **1.2s** (`BG3LE_FAST_ALLOC=1`,
`src/fast_alloc.cpp`):

    fast alloc: active, replacing PhysX TempAllocator at 77/80 sites
    fast alloc (level load): 12270697 served (12270650 recycled),
                             0 passed through, 32.0 MB arena
    Level load took 1.2s after Osiris finished

The conversion work was never the problem. 12.3M allocations per load through
a single global mutex was.

Reproduces on a stock native build with no extender, no mods, outside the
Steam runtime container, on a brand-new game as well as an existing save.
The Windows build under Proton loads the same save in ~15s while running
BG3SE and 57 mods.

## Confirmed: the shipped Linux assets are in Windows format

PhysX serialized blobs are stored uncompressed in the paks, each with a
header naming its version GUID and platform:

    SEBD 77E92B17A4084033A0FDB51332D5A6BB W_64
         ^ PhysX binary version GUID       ^ Windows 64-bit

`Data/Engine.pak` from the **Linux** content depot (2378501) is byte-identical
to the one from the Windows depot (1086941) -- same sha256 -- and all 68 of
its PhysX blobs are tagged `W_64`. Larian cooks physics data on Windows and
ships the same files to both platforms, so the native Linux build converts
every blob on every load.

This also rules out an obvious objection: these measurements were taken with
the Windows `Data/` symlinked in to avoid a 144GB download, but the Linux
depot's copy is identical, so the symlink introduces nothing. The bug is in
the shipped data and affects every native Linux install.

Verify with:

    DepotDownloader -app 1086940 -depot 2378501 \
        -filelist <(echo Data/Engine.pak) -dir /tmp/bg3-linux-data
    grep -a -o -E "[0-9A-F]{32}[A-Z]_[0-9]{2}" /tmp/bg3-linux-data/Data/Engine.pak

## Cause (why the allocator is on the hot path)

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

## Scale, measured

One level load, counters scoped to the load itself:

    432 conversions
    6,384,208 convertClass calls  (6,381,082 outermost -- ~0.05% nested)
    648.65s elapsed inside outermost convertClass
    97.8s wall

648.65s over 97.8s of wall clock is ~6.6 threads' worth, so essentially all
worker-thread time during the stall is spent inside convertClass.

That has to be read alongside the other measurements: CPU sits at ~10% and
every thread samples in futex_wait. Threads cannot be running 6.6 cores'
worth of code and idle at the same time, so the resolution is that they are
*inside* convertClass but *blocked* there, queued on PhysX's shared
TempAllocator. The ~102us mean per conversion supports this -- far too slow
for rewriting one small object's layout, and about right for lock waiting.

So the stall is 6.4M conversions serialised through a single allocator. Both
halves matter: the work is real and the volume enormous, but wall time is
dominated by contention rather than computation. That is why CPU looks idle,
why the stack samples show futex_wait, and why donating timeslices with
sched_yield changed nothing.

Pre-converting the assets to `L_64` would also work, by removing the calls
entirely -- but it is unnecessary. The allocator measurement below shows the
conversion work itself is nearly free.

Timing the allocator settled it: 12,038,602 allocations plus as many frees,
accounting for **99.6%** of all time spent inside `convertClass`. Replacing
the allocator recovers essentially all of it, without touching game data.

## Why the obvious measurements mislead

- **~10% CPU** while "busy": the work is ~6.9M tiny operations serialised
  through PhysX's shared allocator, so threads spend most of their time
  queued rather than running. Stack samples catch them blocked, but the work
  is real. The 450% seen from `/proc` is the uncapped loading screen
  redrawing, not load work.
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
