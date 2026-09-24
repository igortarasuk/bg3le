// Drives bg3se's ImGui overlay.
//
// Everything the overlay needs is already in libbg3le.so: bg3se's
// IMGUIManager, its Vulkan backend, imgui itself and bg3le's field tables for
// every widget type. The one thing it could not do was install its hooks,
// because it wraps seven Vulkan entry points through Detours. It can now --
// see src/detour_interpose.cpp and src/vulkan_forward.cpp -- so this is the
// part that was missing: something to construct the manager and turn it on.
//
// Off unless BG3LE_IMGUI=1, and deliberately so. The hooks sit on
// vkCreateInstance and vkQueuePresentKHR, the overlay does real Vulkan work
// inside the present call, and a mistake there is a crash or a hang in the
// renderer rather than a message in the log. Until it has been through more
// than one machine's driver stack, the default is to leave the engine's
// rendering exactly as it was.
//
// EnableHooks has to run before the game creates its Vulkan instance, and
// IMGUIManager's own containers allocate through the engine's heap -- which is
// not installed until bg3le has read the symbol table, 0.4s after the library
// loads. Constructing in the library constructor threw bad_array_new_length
// every time.
//
// So it is built at the first vkCreateInstance instead. That is after the
// allocator and before the instance exists, which is the window both
// requirements leave, and src/vulkan_forward.cpp calls this from there.
//
// IMGUIManager, VulkanBackend and the widgets are by Norbyte and the bg3se
// contributors (https://github.com/Norbyte/bg3se); only the driving is ours.

#include <stdafx.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/SDLManager.h>
#include <Extender/ScriptExtender.h>

#include <cstdlib>
#include <exception>
#include <memory>

#include "../log.h"

extern "C" bool bg3le_game_allocator_ready();

namespace bg3le {

namespace {

// The manager belongs to the extender object, not to this file.
//
// bg3se's widget code reaches it as gExtender->IMGUI() -- that is how a
// texture or a font gets registered -- so driving a separate instance would
// mean two managers, one drawing and one being written to. This drives the
// one the widgets can see.
bool g_started = false;
bool g_waiting_said = false;

bg3se::extui::IMGUIManager* manager() {
    if (bg3se::gExtender == nullptr) return nullptr;
    return &bg3se::gExtender->IMGUI();
}

}  // namespace

bool imgui_overlay_wanted();
void extender_globals_init();

// Constructs the manager and installs the hooks. Safe to call more than once,
// and deliberately does not latch until it has actually built something: the
// first calls arrive before the engine heap is installed, and giving up on
// those would mean never starting.
void imgui_overlay_start() {
    if (g_started || !imgui_overlay_wanted()) return;

    if (!bg3le_game_allocator_ready()) {
        if (!g_waiting_said) {
            g_waiting_said = true;
            logf("imgui: waiting for the engine heap before building the "
                 "overlay");
        }
        return;
    }
    g_started = true;

    try {
        extender_globals_init();
        auto* ui = manager();
        if (ui == nullptr) {
            logf("imgui: no extender object; the overlay cannot start");
            return;
        }
        ui->EnableHooks();
        ui->EnableUI(true);
        logf("imgui: overlay hooks installed; waiting for the swapchain");
    } catch (std::exception const& e) {
        logf("imgui: could not start the overlay: %s", e.what());
    } catch (...) {
        logf("imgui: could not start the overlay");
    }
}

// Per frame, from the same hook that runs the Lua timers. The backend draws
// from the present hook; this is the manager's own bookkeeping.
void imgui_overlay_tick() {
    if (!g_started) return;
    auto* ui = manager();
    if (ui == nullptr) return;

    try {
        ui->Update();
    } catch (std::exception const& e) {
        logf("imgui: Update failed, stopping the overlay: %s", e.what());
        g_started = false;
    } catch (...) {
        logf("imgui: Update failed, stopping the overlay");
        g_started = false;
    }
}

// Whether the backend has got as far as building its own resources, which is
// the milestone worth reporting: it means the hooks fired and the swapchain
// was recognised.
bool imgui_overlay_ready() {
    auto* ui = manager();
    return ui != nullptr && ui->WasUIInitialized();
}

// Whether the overlay was asked for, got as far as installing its hooks, and
// whether the render backend has built its own resources.
//
// Exposed because the alternative is guessing: bg3se's own IMGUI_DEBUG
// logging is compiled out, so between "hooks installed" and a window
// appearing there is nothing in the log at all.
extern "C" void bg3le_imgui_status(bool* wanted, bool* started,
                                   bool* initialized) {
    if (wanted != nullptr) *wanted = imgui_overlay_wanted();
    if (started != nullptr) *started = g_started;
    if (initialized != nullptr) *initialized = imgui_overlay_ready();
}

}  // namespace bg3le
