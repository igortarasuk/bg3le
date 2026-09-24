# What `Ext.IMGUI` actually needs

Written 2026-09-23, and rewritten the same evening when the first version
turned out to be wrong in the most useful way: the work is far smaller than it
looked.

## The first answer, and why it was wrong

`tools/vendor-exclude.txt` listed
`BG3Extender/Extender/Client/IMGUI/IMGUI.cpp` under "client-UI code that is
not a target yet". Compiling that translation unit alone and listing its
undefined symbols gave a frightening picture — `lua::ImguiHandle`,
`lua::RegistryEntry`, `lua::gStructRegistry`, `ecl::ExtensionState::Get`,
`ProtectedFunctionCallerBase`, `SDLManager` — and the conclusion was that
borrowing it meant replacing bg3le's foundation with bg3se's.

That conclusion was drawn from one object file in isolation. Checking those
288 undefined symbols against what bg3le already builds leaves **37**, of
which **30 are ordinary Vulkan calls** and 7 are libc. Nothing from bg3se's
Lua framework. Nothing from Detours. Nothing from imgui.

The reason is simple: bg3le vendors all of bg3se except a short exclude list,
and `IMGUI.cpp` is not on the list CMake uses — only on the one the
documentation tool reads. So it is already compiled and already linked.
`nm` on `libbg3le.so` finds `extui::IMGUIManager::Update`,
`extui::IMGUIManager::EnableHooks`, thirty-three `VulkanBackend` symbols, and
bg3le's own `FieldTable` re-expansion of every widget type.

imgui is built too, as a static library, with both the Vulkan and the SDL2
backends.

## What is actually missing

One thing: the hooks cannot install.

`VulkanBackend::EnableHooks()` wraps seven Vulkan entry points —
`vkCreateInstance`, `vkCreateDevice`, `vkDestroyDevice`,
`vkCreatePipelineCache`, `vkCreateSwapchainKHR`, `vkDestroySwapchainKHR`,
`vkQueuePresentKHR` — through `WrappedFunction::Wrap`, which calls
`DetourAttachEx`. `vendor/compat/detours.h` is a stub that reports failure,
and `Wrap()` handles failure by clearing itself and logging. So the manager
links, runs, and draws nothing.

## The shape of the fix

Detours patches the call site so the game's call lands on bg3se's hook.
LD_PRELOAD does the same job by a different route, and bg3le already uses it
for Vulkan in `src/vulkan_memory.cpp`: export the symbol from
`libbg3le.so` and the dynamic linker sends the game's call to us.

So a Linux `DetourAttachEx` does not need an instruction-length decoder. It
needs to record, not patch:

    DetourAttachEx(&trampoline, newFunc, &funcTrampoline, ...)
      *funcTrampoline = the original target   -- so operator() reaches it
      detours[original] = newFunc             -- so our export can find it
      return NO_ERROR                         -- so IsWrapped() is true

and then one exported forwarder per hooked function: resolve the real one,
and call the recorded detour if there is one.

That is general rather than IMGUI-specific — it makes `WrappedFunction` work
for anything bg3le is willing to export a forwarder for, which is the thing
`vendor/compat/detours.h` says to do when something genuinely needs it:
"implement these over bg3le's own primitives rather than widening this shim."

## The hooks install now

Done, behind `BG3LE_IMGUI=1`. `src/detour_interpose.cpp` records instead of
patching, `src/vulkan_forward.cpp` exports the seven forwarders, and
`src/vendor/imgui_overlay.cpp` constructs the manager and turns it on. From a
run on 2026-09-23:

    imgui: waiting for the engine heap before building the overlay
    imgui: registered 7 Vulkan forwarders
    detour: hooked 0x7f2fbdbb6090 -> ... (asked for the forwarder at 0x7f2fbf42af60)
    imgui: overlay hooks installed; waiting for the swapchain
    detour: hooked 0x7f2fbdbb5930 -> ...
    detour: hooked 0x7f2fbdbb59d0 -> ...
    detour: hooked 0x7f2fbdbb18d0 -> ...
    detour: hooked 0x7f2fbdbbe170 -> ...
    detour: hooked 0x7f2fbdbb9fd0 -> ...
    detour: hooked 0x7f2fbdbba0f0 -> ...

All seven, in bg3se's own order: the instance hook fires and wraps the device
calls, those wrap the pipeline cache and the swapchain, and the last is
`vkQueuePresentKHR`. bg3se's own chain, driven by interposition.

Two things had to be learnt to get there, both recorded in the source:

**Build it at the first `vkCreateInstance`, not in the library
constructor.** `IMGUIManager`'s containers allocate through the engine's heap,
which bg3le installs 0.4s after the library loads, so constructing early threw
`bad_array_new_length` every time. The first `vkCreateInstance` is after the
allocator and before the instance exists, which is the only window that
satisfies both.

**A forwarder must say what it stands in for.** bg3se asks the loader for
`vkCreateInstance`, is handed a forwarder because the proc-address hook
diverts that name, and registers its replacement against it. The first version
handed that same pointer back as "the original to call through", so the
post-hook called the forwarder, which found the hook, which called the
post-hook — a stack overflow under two hundred frames of `StaticPostHook`.
`detour_register_forwarder` records the pair up front, eagerly, because bg3se
wraps the later entry points from inside the earlier ones' hooks.

## Where it stops

`IMGUIManager::InitializeUI()`, from inside the present hook. It reaches for
bg3se's extender globals:

    io.ConfigDebugHighlightIdConflicts = gExtender->GetConfig().DeveloperMode;
    auto configPath = GetStaticSymbols().ToPath("imgui.ini", UserProfile);
    auto const& language = GetStaticSymbols().GetGlobalSwitches()->Language;

`gExtender` is bg3se's whole extender object and is unresolved here — the link
allows that deliberately, so it is null and the first line faults.
`GetGlobalSwitches()` is null too, and that one is not a small gap: it is the
same object `Ext.Utils.GetGlobalSwitches` refuses over, with no symbol and no
anchor to fingerprint.

So the question this leaves is a real one, and it is a decision rather than a
puzzle: **how much of bg3se's extender globals should bg3le stand up in order
to reuse its UI, against writing bg3le's own overlay over the same imgui?**
Standing up `gExtender` enough to answer `GetConfig()` is small. `Language`
needs an object nobody has located. A bg3le overlay would need the widget
surface written against bg3le's own Lua -- upstream's binding is 70 lines over
440 lines of widget property maps, and bg3le already re-expands those maps
into its own field tables, so the widgets are describable by the machinery
`Ext.Entity` and `Ext.StaticData` already use.

## What depends on it

Mod Configuration Menu's menu, and nothing else in a 57-mod set. MCM's server
side, its settings, its blueprint loading and its net traffic all work —
5eSpells reads every one of its settings through MCM today. What does not work
is drawing the menu, and the `MCM` global its client services reach for, which
its UI initialisation creates.
