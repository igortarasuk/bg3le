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

#include <imgui_internal.h>

#include <Extender/Client/IMGUI/IMGUI.h>
#include <Extender/Client/SDLManager.h>
#include <Extender/ScriptExtender.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>
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
void imgui_api_init();

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
        imgui_api_init();
        logf("imgui: overlay hooks installed; waiting for the swapchain");
    } catch (std::exception const& e) {
        logf("imgui: could not start the overlay: %s", e.what());
    } catch (...) {
        logf("imgui: could not start the overlay");
    }
}

// What the last frame drew, so a test can tell a widget tree that renders
// from one that is merely attached. Headless has no screen to look at.
std::atomic<std::uint64_t> g_frames{0};
std::atomic<int> g_last_vertices{0};
std::atomic<int> g_last_lists{0};
std::atomic<int> g_imgui_frames{0};
std::atomic<float> g_mouse_x{0};
std::atomic<float> g_mouse_y{0};
std::atomic<float> g_display_w{0};
std::atomic<float> g_display_h{0};
std::atomic<bool> g_mouse_down{false};
std::atomic<bool> g_want_mouse{false};
std::atomic<bool> g_hovered_window{false};
std::atomic<bool> g_nav_no_hover{false};

void record_frame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);

    // imgui's own count, which only advances if Update got past its early
    // return and reached NewFrame. Without it there is no telling a tick
    // that drew nothing from one that never drew.
    if (ImGui::GetCurrentContext() != nullptr) {
        g_imgui_frames.store(ImGui::GetFrameCount(),
                             std::memory_order_relaxed);
    }

    // Where imgui thinks the mouse is, and how big it thinks the screen is.
    // Both are the SDL side's to supply, and both were zero while that was a
    // stub -- worth reporting rather than inferring from a vertex count.
    if (ImGui::GetCurrentContext() != nullptr) {
        auto const& io = ImGui::GetIO();
        g_mouse_x.store(io.MousePos.x, std::memory_order_relaxed);
        g_mouse_y.store(io.MousePos.y, std::memory_order_relaxed);
        g_display_w.store(io.DisplaySize.x, std::memory_order_relaxed);
        g_display_h.store(io.DisplaySize.y, std::memory_order_relaxed);
        g_mouse_down.store(io.MouseDown[0], std::memory_order_relaxed);

        // Whether imgui thinks the pointer is over one of its windows, and
        // whether it has turned mouse hovering off because navigation is
        // being driven from the keyboard. Between them they say why an
        // item that is plainly under the cursor does not report as hovered.
        g_want_mouse.store(io.WantCaptureMouse, std::memory_order_relaxed);
        auto* gui = ImGui::GetCurrentContext();
        g_hovered_window.store(gui->HoveredWindow != nullptr,
                               std::memory_order_relaxed);
        g_nav_no_hover.store(gui->NavHighlightItemUnderNav,
                             std::memory_order_relaxed);
    }

    auto* draw = ImGui::GetDrawData();
    if (draw == nullptr) return;
    g_last_vertices.store(draw->TotalVtxCount, std::memory_order_relaxed);
    g_last_lists.store(draw->CmdListsCount, std::memory_order_relaxed);
}

// Mouse input handed straight to imgui, for driving the overlay without a
// mouse.
//
// Not the same thing as SDLManager::InjectEvent, which feeds the game's own
// event loop and deliberately bypasses imgui. These are applied on the
// render thread, immediately before the frame that will see them, which is
// the only thread allowed to touch imgui's IO.
// One button change, at a position. Applied one per frame, because imgui
// trickles its own event queue for exactly this reason: two button changes
// in one frame are one click as far as a widget is concerned, so a sweep
// that queued them all would register once.
struct MouseInput {
    float X{0};
    float Y{0};
    int Button{0};
    bool Down{false};
};

std::mutex& input_lock() {
    static std::mutex m;
    return m;
}

std::deque<MouseInput>& pending_input() {
    static std::deque<MouseInput> queue;
    return queue;
}

// Where the mouse is held, if it is. Sticky rather than one event, because
// the SDL backend sets the position every frame from the real mouse and
// would otherwise put it back.
bool g_holding = false;
float g_hold_x = 0;
float g_hold_y = 0;

// Applied from SDLManager::NewFrame, after the SDL backend has had its say.
//
// Order matters and cost a diagnosis: queued before the backend, an injected
// position is simply overwritten by whatever the real mouse is doing, and
// nothing registers as hovered. After it, these are the last word on the
// frame.
void apply_input() {
    bool holding = false;
    float holdX = 0;
    float holdY = 0;
    bool haveInput = false;
    MouseInput input;
    {
        const std::lock_guard<std::mutex> held(input_lock());
        holding = g_holding;
        holdX = g_hold_x;
        holdY = g_hold_y;
        if (!pending_input().empty()) {
            input = pending_input().front();
            pending_input().pop_front();
            haveInput = true;

            // A button change carries its own position, and it becomes the
            // held one: a widget reads the mouse where it was left.
            g_holding = true;
            g_hold_x = input.X;
            g_hold_y = input.Y;
            holding = true;
            holdX = input.X;
            holdY = input.Y;
        }
    }

    if (!holding && !haveInput) return;

    auto& io = ImGui::GetIO();

    // imgui ignores input to an unfocused application, and a headless run
    // has no focus of its own to give it.
    io.AddFocusEvent(true);

    if (holding) io.AddMousePosEvent(holdX, holdY);
    if (haveInput) io.AddMouseButtonEvent(input.Button, input.Down);
}

void imgui_apply_injected_input() {
    if (ImGui::GetCurrentContext() == nullptr) return;
    apply_input();
}

// Per frame, from the same hook that runs the Lua timers. The backend draws
// from the present hook; this is the manager's own bookkeeping.
void imgui_overlay_tick() {
    if (!g_started) return;
    auto* ui = manager();
    if (ui == nullptr) return;

    try {
        ui->Update();
        record_frame();
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
// Holds the mouse at a position, or stops holding it.
extern "C" void bg3le_imgui_hold_mouse(float x, float y, bool holding) {
    const std::lock_guard<std::mutex> held(bg3le::input_lock());
    bg3le::g_holding = holding;
    bg3le::g_hold_x = x;
    bg3le::g_hold_y = y;
}

// A press and a release at a position, each taking its own frame.
extern "C" void bg3le_imgui_click_at(float x, float y, int button) {
    const std::lock_guard<std::mutex> held(bg3le::input_lock());
    bg3le::pending_input().push_back(bg3le::MouseInput{x, y, button, true});
    bg3le::pending_input().push_back(bg3le::MouseInput{x, y, button, false});
}

// Where imgui thinks the mouse is, how big the display is, and whether the
// left button is down.
extern "C" void bg3le_imgui_input_state(float* mouseX, float* mouseY,
                                        float* displayW, float* displayH,
                                        bool* mouseDown, bool* wantMouse,
                                        bool* hoveredWindow,
                                        bool* navNoHover) {
    *wantMouse = bg3le::g_want_mouse.load(std::memory_order_relaxed);
    *hoveredWindow = bg3le::g_hovered_window.load(std::memory_order_relaxed);
    *navNoHover = bg3le::g_nav_no_hover.load(std::memory_order_relaxed);
    *mouseX = bg3le::g_mouse_x.load(std::memory_order_relaxed);
    *mouseY = bg3le::g_mouse_y.load(std::memory_order_relaxed);
    *displayW = bg3le::g_display_w.load(std::memory_order_relaxed);
    *displayH = bg3le::g_display_h.load(std::memory_order_relaxed);
    *mouseDown = bg3le::g_mouse_down.load(std::memory_order_relaxed);
}

extern "C" void bg3le_imgui_frame_stats(std::uint64_t* frames,
                                        int* vertices, int* lists,
                                        int* drawnFrames) {
    if (drawnFrames != nullptr) {
        *drawnFrames = bg3le::g_imgui_frames.load(std::memory_order_relaxed);
    }
    if (frames != nullptr) {
        *frames = bg3le::g_frames.load(std::memory_order_relaxed);
    }
    if (vertices != nullptr) {
        *vertices = bg3le::g_last_vertices.load(std::memory_order_relaxed);
    }
    if (lists != nullptr) {
        *lists = bg3le::g_last_lists.load(std::memory_order_relaxed);
    }
}

extern "C" void bg3le_imgui_status(bool* wanted, bool* started,
                                   bool* initialized) {
    if (wanted != nullptr) *wanted = imgui_overlay_wanted();
    if (started != nullptr) *started = g_started;
    if (initialized != nullptr) *initialized = imgui_overlay_ready();
}

}  // namespace bg3le
