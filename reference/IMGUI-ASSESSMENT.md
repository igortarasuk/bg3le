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

That question was answered by standing the globals up: `gExtender` is
constructed for real, because bg3se's widget code reaches the manager as
`gExtender->IMGUI()` and the manager bg3le drives has to be the one the
widgets can see. `GetConfig()` needed nothing. `ToPath` returns an empty
string without path roots and upstream's caller already handles that.
`GetGlobalSwitches()` is bg3le's own default object, and the only thing read
from it is a language, to pick a font size -- pointing it at an object found
on a layout that does not match this build would be worse than defaulting.

## What it took after that

Four things, and three of them were found by counting vertices rather than by
looking at a screen, because this runs headless.

**A widget tree.** `IMGUIManager` takes an `IMGUIObjectManager` through
`SetObjects` and nothing was giving it one, so there was nowhere for a window
to be. The first window's handle is zero -- `Window` is the first
`IMGUIObjectType` and a fresh pool slot starts at salt zero -- so only
`InvalidHandle` means failure.

**The client extension state.** `IMGUIObjectManager::ClientUpdate` pins it
every frame and `ecl::ExtensionState::Get()` asserts on a null
`unique_ptr`, so the first frame after a widget tree existed killed the game
with SIGILL.

**The fonts.** `LoadFont` reads the game's own TTF through
`script::LoadExternalFile`, which needs `ls::FileReader`'s constructor, and
no engine function in this build carries a symbol to call. The atlas stayed
empty -- and Norbyte's imgui fork has `AddFontDefault()` disabled, so an
empty atlas is not a smaller overlay, it is no overlay: zero vertices a
frame. bg3le reads the archives itself, indexed by the priority byte in each
LSPK header, which is how the engine resolves them.

**The SDL side.** `SDLManager` was a stub, so imgui had no viewport and no
input: everything drew into a zero-sized display and 58 vertices survived
clipping. With the window bound at 1280x720, a window with text and four
widgets draws 878.

## The two that only showed up in input

Both were found by checking against bg3se rather than by reasoning about
bg3le's own code, which is the lesson worth keeping.

**The overlay was drawn from the wrong thread.** Upstream calls `Update()`
from `ecl::ScriptExtender::OnUpdate` -- the client's own update, on the
thread that also polls SDL. bg3le called it from
`esv::GameServer::UpdateMessagesToSend`, the server's story tick. Drawing
survived that; input did not, because imgui's event queue was appended on one
thread and consumed on another.

**Nothing flushed the callback queue.** A widget fires by pushing onto
`IMGUIObjectManager`'s `DeferredLuaDelegateQueue`, and upstream drains it in
`ClientUpdate()` -- but only after pinning its own client Lua state, which
bg3le never attaches. The pin was always false, so every click sat in that
queue for the life of the process.

Between them these cost several hours of measuring imgui's hit-testing --
clip rects, item rectangles, nav state, font scale -- none of which was ever
wrong. The widget was always laid out correctly; the frame that submitted it
had simply never seen the mouse, and nothing was delivering what it queued.

## How a delegate works here

Upstream's `LuaDelegate` holds a `lua::RegistryEntry`, which finds its
manager through `lua::State::FromLua(L)` -- bg3se's own Lua state, which
bg3le never starts. And calling one marshals the arguments through bg3se's
userdata machinery, whose metatables are registered during that same state's
init, so a widget would reach Lua as an object with no methods on it.

So `vendor/bg3se/BG3Extender/Lua/Shared/LuaDelegate.h` is one of the three
places bg3le changed upstream's behaviour rather than its syntax. A delegate
is an id into bg3le's own table; `Call` copies the arguments into a queue,
and `Ext.IMGUI` drains it from the tick of the context that registered the
callback. See `vendor/NOTICE.md` and `src/vendor/imgui_events.cpp`.

## What depends on it

Mod Configuration Menu's menu, and nothing else in a 57-mod set. MCM's server
side, its settings, its blueprint loading and its net traffic all worked
before any of this — 5eSpells reads every one of its settings through MCM
today. What was missing was drawing the menu.

## Verified

Against a live game, with mouse input injected because there is no screen to
click on:

- all thirty `Add*` kinds, `AddTabItem`, `AddColumn`, `AddRow`, `AddCell`,
  `SetOpen`, `Open`, `AddItem`, `AddMainMenu`, `Tooltip`, `Activate`,
  `GetChildren`, and `SetStyle`/`GetStyle` and `SetColor`/`GetColor`
  round-tripping
- `OnClick`, `OnRightClick` and `OnHoverEnter` from injected mouse input;
  a checkbox's `OnChange` arriving with its new value, and the widget's own
  `Checked` reading back `true` afterwards
- nav activation through `Activate()` firing `OnActivate`, `OnClick` and
  `OnDeactivate`, with the widget handle passed through
- `AbsolutePosition` written, read back and cleared -- and the write having
  a real effect: after it a click at the new screen position fires the button
  and one at the old position does not
- `GetViewportSize()` reporting 1280x720

## Still open

The `MCM` global that MCM's client services reach for is created by its UI
initialisation, so whether MCM's menu comes up under bg3le has not been
tested end to end. That is the next thing to try, and it is a test rather
than a piece of work.
