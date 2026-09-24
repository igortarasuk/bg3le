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

#include <cstdlib>
#include <exception>
#include <memory>

#include "../log.h"

extern "C" bool bg3le_game_allocator_ready();

namespace bg3le {

namespace {

// Held rather than static-initialised: the manager touches imgui, and imgui
// must not be built before the library has finished loading.
std::unique_ptr<bg3se::SDLManager> g_sdl;
std::unique_ptr<bg3se::extui::IMGUIManager> g_ui;
bool g_started = false;
bool g_waiting_said = false;

}  // namespace

bool imgui_overlay_wanted();

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
        g_sdl = std::make_unique<bg3se::SDLManager>();
        g_ui = std::make_unique<bg3se::extui::IMGUIManager>(*g_sdl);
        g_ui->EnableHooks();
        logf("imgui: overlay hooks installed; waiting for the swapchain");
    } catch (std::exception const& e) {
        logf("imgui: could not start the overlay: %s", e.what());
        g_ui.reset();
        g_sdl.reset();
    } catch (...) {
        logf("imgui: could not start the overlay");
        g_ui.reset();
        g_sdl.reset();
    }
}

// Per frame, from the same hook that runs the Lua timers. The backend draws
// from the present hook; this is the manager's own bookkeeping.
void imgui_overlay_tick() {
    if (g_ui == nullptr) return;

    try {
        g_ui->Update();
    } catch (std::exception const& e) {
        logf("imgui: Update failed, stopping the overlay: %s", e.what());
        g_ui.reset();
    } catch (...) {
        logf("imgui: Update failed, stopping the overlay");
        g_ui.reset();
    }
}

// Whether the backend has got as far as building its own resources, which is
// the milestone worth reporting: it means the hooks fired and the swapchain
// was recognised.
bool imgui_overlay_ready() {
    return g_ui != nullptr && g_ui->WasUIInitialized();
}

}  // namespace bg3le
