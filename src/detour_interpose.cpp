// Detours, by recording rather than by patching.
//
// bg3se hooks a function with CoreLib/Wrappers.h, which calls DetourAttachEx
// to rewrite the target's first instructions so the call lands on its own
// function, and hands back a trampoline that reaches the original. That needs
// an instruction-length decoder, which bg3le has deliberately avoided.
//
// It does not need one, because on Linux there is a second way to make a call
// land somewhere else: export the symbol from libbg3le.so and the dynamic
// linker sends the game's call to us. bg3le already does that for Vulkan in
// src/vulkan_memory.cpp.
//
// So this half of the job is bookkeeping. DetourAttachEx is told the target
// and the replacement; it records the pair and reports success, and hands the
// target back as the "trampoline" so WrappedFunction's operator() reaches the
// original exactly as upstream expects -- nothing was overwritten, so there
// is nothing to trampoline past. The other half is one exported forwarder per
// hooked function, asking detour_for() whether a replacement is registered
// against the real function; src/vulkan_forward.cpp has those for the seven
// entry points the ImGui overlay wraps.
//
// What this cannot give you is a hook on a function bg3le exports no
// forwarder for. That is the honest limit, and both sides log themselves so a
// mismatch is visible: a silent no-op is how the ImGui overlay came to link,
// run and draw nothing for as long as it did.
//
// The callers are by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); this implementation is ours.

#include "detour_interpose.h"

#include <cstddef>
#include <mutex>
#include <unordered_map>

#include "log.h"

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

// Original function -> the replacement bg3se wants called instead. Keyed by
// the loader's function, never by a forwarder.
std::unordered_map<void const*, void*>& detours() {
    static std::unordered_map<void const*, void*> map;
    return map;
}

// bg3le's exported forwarder -> the loader's function behind it.
std::unordered_map<void const*, void*>& forwarders() {
    static std::unordered_map<void const*, void*> map;
    return map;
}

// The loader's function behind an address, which is itself if it is not one
// of ours. Caller holds the lock.
void* behind(void const* address) {
    auto found = forwarders().find(address);
    if (found != forwarders().end()) return found->second;
    return const_cast<void*>(address);
}

}  // namespace

namespace bg3le {

void* detour_for(void const* original) {
    if (original == nullptr) return nullptr;

    const std::lock_guard<std::mutex> held(lock());
    auto found = detours().find(original);
    return found != detours().end() ? found->second : nullptr;
}

void detour_register_forwarder(void* forwarder, void* real) {
    if (forwarder == nullptr || real == nullptr || forwarder == real) return;

    const std::lock_guard<std::mutex> held(lock());
    forwarders()[forwarder] = real;
}

std::size_t detour_count() {
    const std::lock_guard<std::mutex> held(lock());
    return detours().size();
}

}  // namespace bg3le

// ppPointer holds the target on entry. Upstream reads *ppRealTrampoline
// afterwards and calls through it to reach the original, and treats a
// non-zero return as "not wrapped".
extern "C" long DetourAttachEx(void** ppPointer, void* pDetour,
                               void** ppRealTrampoline, void** ppRealTarget,
                               void** ppRealDetour) {
    if (ppPointer == nullptr || *ppPointer == nullptr || pDetour == nullptr) {
        return 87;  // ERROR_INVALID_PARAMETER
    }

    void* asked = *ppPointer;
    void* original = nullptr;
    {
        const std::lock_guard<std::mutex> held(lock());
        // If bg3se was handed a forwarder, the hook belongs to the function
        // behind it -- and the trampoline has to be that function, or calling
        // through it comes straight back here.
        original = behind(asked);
        detours()[original] = pDetour;
    }
    if (original != asked) {
        bg3le::logf("detour: hooked %p -> %p (asked for the forwarder at %p)",
                    original, pDetour, asked);
    } else {
        bg3le::logf("detour: hooked %p -> %p", original, pDetour);
    }

    if (ppRealTrampoline != nullptr) *ppRealTrampoline = original;
    if (ppRealTarget != nullptr) *ppRealTarget = original;
    if (ppRealDetour != nullptr) *ppRealDetour = pDetour;
    return 0;  // NO_ERROR
}

extern "C" long DetourDetach(void** ppPointer, void* pDetour) {
    if (ppPointer == nullptr || *ppPointer == nullptr) return 87;

    const std::lock_guard<std::mutex> held(lock());
    auto found = detours().find(behind(*ppPointer));
    if (found == detours().end() || found->second != pDetour) return 87;
    detours().erase(found);
    return 0;
}
