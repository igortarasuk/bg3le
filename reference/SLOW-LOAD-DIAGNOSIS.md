# Native Linux build: 65-78s stall at 75% on every level load

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

## Scale, measured

Instrumenting the conversion path (`BG3LE_PHYSX_PROBE=1`) for one level load:

    641 conversions
    6,874,980 convertClass calls

Those counts are solid. The timing from that run was not: 566s against 75.7s
of wall clock, which is impossible as stated. Two defects, both mine --
counters accumulated from probe install (including the pre-menu stall and
menu time, not just the level load), and convertClass recurses, so timing
every invocation counted the same interval once per nesting level.

Both are fixed: counters reset when the level load begins, and only the
outermost call per thread is timed. Times are still summed across worker
threads, so they legitimately exceed wall time -- but by a factor bounded by
thread count rather than by recursion depth.

What stands regardless of timing: 641 serialized collections holding ~6.9M
objects have their memory layout rewritten on every load. Pre-converting
those 641 collections to `L_64` removes that work.

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
