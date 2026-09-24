# memsteer

Keeps a native Vulkan game's per-frame uploads out of BAR-mapped VRAM.

This is bg3le's `src/vulkan_memory.cpp` built on its own, so it can be pointed
at any native Vulkan game rather than only at Baldur's Gate 3. It is the same
file the extender links — one implementation, so the two cannot drift.

    cmake --build build --target memsteer
    env MEMSTEER=on LD_PRELOAD=/path/to/build/memsteer.so ./the-game

## What it does, and why it is conditional

On a machine whose GPU is external, the narrowest hop on the way to it can be
very slow — 2.5 GT/s x1, about 250 MB/s. With resizable BAR the driver offers a
`DEVICE_LOCAL | HOST_VISIBLE` heap, and CPU writes into it cross that hop
synchronously. Measured on the machine this was written for: **0.21 GB/s**
there against **14.68 GB/s** into ordinary host memory, seventy times slower.

A Vulkan engine that streams per-frame data through that heap spends its main
thread in `memcpy` while the GPU starves. Hiding `HOST_VISIBLE` from the
device-local memory types makes the engine's own selection logic pick host
memory instead. It still chooses; the slow option is simply no longer on the
menu.

On Baldur's Gate 3, on that machine: **30.7 → 71.0 fps**, p99 frametime
184.2ms → 15.8ms, GPU busy 57% → 99%, total CPU 42-55% → 34%.

On a desktop with a real x16 link, writes into that heap are fast and steering
away from it would throw away a genuine optimisation. So the default measures
the hardware rather than assuming: it times writes into every host-visible
memory type at startup (about 0.43s) and only steers when device-local
host-visible memory is more than four times slower than host memory. On a
normal machine it is a no-op and says so.

## Options

| Variable | Effect |
| --- | --- |
| `MEMSTEER=on` | Steer without measuring. |
| `MEMSTEER=off` | Do nothing. |
| `MEMSTEER` unset or anything else | Measure, then decide. |
| `MEMSTEER_LOG=<path>` | Write the log there instead of stderr. |

A game launched through Steam usually has nowhere useful for stderr to go, so
`MEMSTEER_LOG` is how to see what it decided.

## Put it on the game, not on a wrapper

`LD_PRELOAD` is inherited, and this hides memory types that other Vulkan
clients need. Exporting it broke gamescope outright — its own renderer failed
with `findMemoryType failed` and came up with no backend. Set it on the game
process only:

    env MEMSTEER=on LD_PRELOAD=.../memsteer.so ./the-game    # right
    export LD_PRELOAD=.../memsteer.so; gamescope -- ./the-game   # wrong

## How the hook works

The interposed symbols are `vkGetInstanceProcAddr` and `vkGetDeviceProcAddr`
as well as `vkGetPhysicalDeviceMemoryProperties` and its `2`/`2KHR` forms. A
Vulkan application asks the loader for function pointers and calls through
those, so interposing the memory-properties entry points alone reaches nothing
at all in an engine that does it properly. The proc-address hooks hand back
our pointers, and everything else follows from that.

## Link the loader, or every pointer is null

`memsteer.so` links `libvulkan.so.1` rather than only `dl`, and the reason is
worth knowing before anyone trims it. The hooks reach the real functions with
`dlsym(RTLD_NEXT, ...)`. If nothing has pulled the Vulkan loader in by the
time the engine asks for its first proc address, that returns null — and then
*every* pointer handed back is null. The application takes the crash in its
own code, three frames from `main`, with nothing of this library on the stack.

That is exactly what the first standalone build did. `real()` now asks the
loader directly when `RTLD_NEXT` misses, and says so in the log, so the same
mistake cannot be silent again.

## Status

Verified standalone against Baldur's Gate 3 on 2026-09-23, with no extender
loaded at all:

    vkmem: type 2 writes at 37.81 GB/s
    vkmem: type 3 writes at 0.12 GB/s (device-local)
    vkmem: type 4 writes at 0.12 GB/s (device-local)
    vkmem: type 9 writes at 0.12 GB/s (device-local)
    vkmem: host 37.81 GB/s, device-local host-visible 0.12 GB/s
           -- steering uploads to host memory
    vkmem: hid HOST_VISIBLE from 3 device-local memory types

Three hundred and fifteen to one, against a threshold of four to one. It
measured, decided and steered on a game that knows nothing about it, which is
the whole point of extracting it.

Not yet tried on a second game. Shadow of Mordor is the next case.
