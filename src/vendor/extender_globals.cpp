// The two bg3se globals its own UI code reaches through, stood up for bg3le.
//
// bg3se's ImGui overlay is compiled into libbg3le.so and its hooks install
// (see src/detour_interpose.cpp), but IMGUIManager::InitializeUI faulted
// immediately on `gExtender->GetConfig()` -- bg3le never creates a
// ScriptExtender, so gExtender is null -- and would have faulted again on
// `GetStaticSymbols()`, which dereferences a null gStaticSymbols.
//
// Neither needs much. What the overlay actually reads is small and, apart
// from the manager itself, cosmetic:
//
//   gExtender->IMGUI()                        the manager -- this matters
//   gExtender->GetConfig().DeveloperMode      two imgui debug flags
//   GetStaticSymbols().ToPath("imgui.ini")    where imgui saves window
//                                             positions; returns "" without
//                                             path roots, which upstream's
//                                             own code already handles
//   GetGlobalSwitches()->Language             whether to drop the smallest
//                                             font sizes, for CJK glyphs
//
// The manager is the reason a real ScriptExtender is constructed rather than
// a stub: the widget code reaches it as gExtender->IMGUI(), so the manager
// bg3le drives and the manager the widgets register textures and fonts with
// have to be the same object. Constructing one is cheap -- its members'
// constructors are empty and it allocates a console -- and nothing calls
// Initialize() or PostStartup(), which are where upstream's real work is.
//
// GlobalSwitches is bg3le's own default until the engine's is located, and
// that is deliberate rather than a placeholder: the only thing read from it
// here is the language, to decide a font size. Answering that from a default
// is honest; pointing it at an object found on a layout that does not match
// this build would not be. See reference/GLOBAL-SWITCHES.md, and
// bg3le_set_global_switches below for where the real one goes when it is
// found.
//
// ScriptExtender, StaticSymbols and GlobalSwitches are by Norbyte and the
// bg3se contributors (https://github.com/Norbyte/bg3se); standing them up
// here is ours.

#include <stdafx.h>

#include <Extender/ScriptExtender.h>
#include <GameDefinitions/Symbols.h>

#include <memory>
#include <mutex>

#include "../log.h"

namespace bg3le {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

bool g_ready = false;

// What GetGlobalSwitches() hands back. Starts as bg3le's own default and is
// repointed if the engine's is ever confirmed.
bg3se::GlobalSwitches* g_switches = nullptr;

bg3se::GlobalSwitches& default_switches() {
    static bg3se::GlobalSwitches fallback{};
    return fallback;
}

}  // namespace

// Creates gExtender and gStaticSymbols if they are not there yet.
//
// Safe to call more than once and from either context; the overlay calls it
// before it builds anything.
void extender_globals_init() {
    const std::lock_guard<std::mutex> held(lock());
    if (g_ready) return;
    g_ready = true;

    if (bg3se::gStaticSymbols == nullptr) {
        // Every member stays null. That is not a gap: each accessor on it
        // checks, and ToPath says so and returns an empty string, which is
        // what upstream's caller already expects when path roots are absent.
        bg3se::gStaticSymbols = new bg3se::StaticSymbols();
    }

    if (g_switches == nullptr) g_switches = &default_switches();
    bg3se::gStaticSymbols->ls__GlobalSwitches = &g_switches;

    if (bg3se::gExtender == nullptr) {
        bg3se::gExtender = std::make_unique<bg3se::ScriptExtender>();
    }

    logf("extender: globals stood up (config and static symbols; global "
         "switches are bg3le's default until the engine's is confirmed)");
}

// Points GetGlobalSwitches() at the engine's own object.
//
// Nothing calls this yet -- the search in src/vendor/global_switches.cpp
// refuses, because bg3se's declared layout is not this build's. It is here so
// that locating the object is the only work left, rather than locating it and
// then finding out where it has to be plumbed.
void extender_set_global_switches(void* engineSwitches) {
    const std::lock_guard<std::mutex> held(lock());
    g_switches = engineSwitches != nullptr
                     ? (bg3se::GlobalSwitches*)engineSwitches
                     : &default_switches();
    logf("extender: global switches now %s",
         engineSwitches != nullptr ? "the engine's" : "bg3le's default");
}

}  // namespace bg3le
