# Why `Ext.IMGUI` cannot be borrowed

Written 2026-09-23. `tools/vendor-exclude.txt` listed
`BG3Extender/Extender/Client/IMGUI/IMGUI.cpp` under "client-UI code that is
not a target yet", which is true and says nothing about how much work it
would be. This is the measurement.

## It compiles

The whole translation unit — 2,305 lines, including the 866-line Vulkan
backend in `Vulkan.inl` — compiles clean under bg3le's own flags: clang,
libc++, `-std=gnu++23`, the same `msvc_compat.h` prefix header everything
else in `vendor/` uses. No errors, no warnings worth the name.

So the obstacle is not the language, the toolchain, or Windows-only syntax.

## What it needs at link time

Four groups. Three are ordinary:

**imgui itself**, which is already vendored at
`external/third_party/imgui` — core, `imgui_draw`, `imgui_tables`,
`imgui_widgets` and `backends/imgui_impl_vulkan.cpp` are all present, they
are simply not compiled into anything yet.

**The Vulkan entry points** it hooks: `vkCreateInstance`, `vkCreateDevice`,
`vkDestroyDevice`, `vkCreatePipelineCache`, `vkCreateSwapchainKHR`,
`vkDestroySwapchainKHR`, `vkQueuePresentKHR`. Every one is the same shape as
the calls `src/vulkan_memory.cpp` already interposes, so this is a solved
problem here.

**Detours** — `DetourAttachEx`, `DetourTransactionBegin` and friends. Windows
inline hooking, used only to install the hooks above. bg3le does that with
PLT interposition and `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`
diversion, which is strictly easier.

## What it actually needs, and why that is the answer

The fourth group is the one that decides it. `IMGUI.cpp` is written against
bg3se's Lua binding framework, not against Lua:

    bg3se::ecl::ExtensionState::Get()
    bg3se::ExtensionStateBase::IncLuaRefs / DecLuaRefs
    bg3se::lua::RegistryEntry
    bg3se::lua::gStructRegistry
    bg3se::lua::ImguiHandle
    bg3se::lua::push(lua_State*, ...)            -- the overload set
    bg3se::lua::ProtectedFunctionCallerBase::ProtectedCall
    bg3se::lua::lua_enter_pcallk, EnterVMCheck, TracebackHandler
    bg3se::lua::DeferredLuaDelegateQueue::Flush
    bg3se::SDLManager::InitializeUI / NewFrame / DestroyUI
    bg3se::gStaticSymbols, bg3se::GFS

Every widget is a `lua::ImguiHandle` held in a `lua::RegistryEntry`, every
property goes through `lua::gStructRegistry`, and every callback re-enters
Lua through `ProtectedFunctionCallerBase` against an
`ecl::ExtensionState`. `SDLManager` is on the exclude list too, for its own
reasons.

bg3le does not have any of that. Its Lua layer is its own — see the README
on why the runtime property maps are not usable here — and the two contexts
are plain `lua_State`s, not `lua::State` objects with lifetimes and a struct
registry. Linking bg3se's IMGUI means bringing in bg3se's whole Lua proxy
framework and its client extension state, which is not a feature addition to
bg3le; it is replacing bg3le's foundation with bg3se's.

## So what would it take

bg3le's own implementation, over the same imgui:

1. imgui core plus `imgui_impl_vulkan` built as a library (an afternoon, no
   risk, nothing else depends on it).
2. An overlay in the game's swapchain: interpose `vkCreateSwapchainKHR` and
   `vkQueuePresentKHR` the way `src/vulkan_memory.cpp` interposes the memory
   calls, render imgui's draw data into the presented image. This is the part
   bg3se's `Vulkan.inl` is a good reference for even though it cannot be
   linked — the sequence is the same.
3. Input: the game is SDL2, so `SDL_PollEvent` is interposable, and imgui
   ships an SDL2 backend.
4. The widget tree and `Ext.IMGUI` itself, against bg3le's own Lua
   bindings. Upstream's surface is in `Lua/Libs/ClientIMGUI.inl` (small) and
   `GameDefinitions/PropertyMaps/IMGUI.inl` (440 lines of widget
   properties), which is the part that has to be matched name for name.

Step 4 is the bulk, and it is the one with a design question in it: how much
of upstream's widget surface to carry, and whether the overlay owns input
when it is open. That is worth deciding deliberately rather than discovering
halfway through.

## What depends on it

Mod Configuration Menu's menu. Its server side, its settings, its blueprint
loading and its net traffic all work — 5eSpells reads every one of its
settings through MCM today. What does not work is drawing the menu, and the
`MCM` global its client services reach for, which its UI initialisation
creates. That is the only thing in the 57-mod set known to need `Ext.IMGUI`.
