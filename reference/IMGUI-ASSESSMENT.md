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

## What is left after that

`Ext.IMGUI` itself, in bg3le's Lua. Upstream's binding is
`Lua/Libs/ClientIMGUI.inl`, which is 70 lines, over a widget surface
described in `GameDefinitions/PropertyMaps/IMGUI.inl` — and bg3le already
re-expands those property maps into its own field tables, so the widgets are
describable by the machinery `Ext.Entity` and `Ext.StaticData` already use.

## What depends on it

Mod Configuration Menu's menu, and nothing else in a 57-mod set. MCM's server
side, its settings, its blueprint loading and its net traffic all work —
5eSpells reads every one of its settings through MCM today. What does not work
is drawing the menu, and the `MCM` global its client services reach for, which
its UI initialisation creates.
