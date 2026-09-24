// Keeps the engine's per-frame uploads out of BAR-mapped VRAM.
//
// On this machine the GPU is external, and the narrowest hop on the way to it
// runs at 2.5 GT/s x1 -- about 250 MB/s. With resizable BAR the driver offers
// a 16GB DEVICE_LOCAL|HOST_VISIBLE heap, and CPU writes into it cross that
// hop synchronously. Measured: 0.21 GB/s there against 14.68 GB/s into
// ordinary host memory, seventy times slower.
//
// BG3's Vulkan backend streams per-frame data through that heap, so its main
// thread sat in memcpy -- about 11ms of a 30ms frame for roughly 2.3MB of
// data -- while the GPU starved at 57%. Hiding HOST_VISIBLE from the
// device-local types makes the engine's own selection logic pick host memory
// instead. It still chooses; the slow option is simply no longer on the menu.
//
// Measured on the save this was found with: 30.7fps -> 71.0fps, p99 frametime
// 184.2ms -> 15.8ms, GPU busy 57% -> 99%, and total CPU down from 42-55% to
// 34%. The main thread stopped blocking in futex_wait and now waits in
// drm_syncobj_array_wait_timeout, which is what a healthy pipeline does: work
// finished, waiting on the GPU. It also beats the DX11 build under Proton,
// which reached about 50fps by avoiding the heap but paying translation.
//
// This is conditional on purpose. On a desktop with a real x16 link, writes
// into that heap are fast and steering away from it would throw away a
// genuine optimisation, so the decision is made by measuring the hardware
// rather than by assuming. MEMSTEER=off disables it, =on forces it, and the
// default measures; BG3LE_VKMEM is accepted under bg3le for the same three.
//
// Built twice from this one file: into libbg3le.so, and into memsteer.so for
// any other native Vulkan game on the same hardware. See MEMSTEER.md.
//
// The hook is on vkGetInstanceProcAddr and vkGetDeviceProcAddr rather than on
// the entry points themselves: a Vulkan application asks the loader for
// function pointers and calls through those, so interposing the exported
// symbols alone reaches nothing at all.

#include <dlfcn.h>
#include <vulkan/vulkan.h>

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "log.h"

namespace {

using bg3le::logf;

enum class Mode { Measure, Force, Off };

// Two names for one switch. MEMSTEER is what the standalone tool reads --
// this file is built twice, once into libbg3le.so and once into memsteer.so
// for other native Vulkan games -- and BG3LE_VKMEM is the name bg3le's own
// documentation uses. Whichever is set wins; MEMSTEER first, so a launcher
// wrapping several games can set one variable.
char const* const kSwitchNames[] = {"MEMSTEER", "BG3LE_VKMEM"};

Mode mode() {
    static const Mode m = [] {
        for (char const* name : kSwitchNames) {
            const char* opt = std::getenv(name);
            if (opt == nullptr) continue;
            if (std::strcmp(opt, "off") == 0) return Mode::Off;
            if (std::strcmp(opt, "on") == 0) return Mode::Force;
            return Mode::Measure;
        }
        return Mode::Measure;
    }();
    return m;
}

// The loader's own function, past ours.
//
// RTLD_NEXT alone is not enough, and the failure is silent and expensive:
// if the Vulkan loader is not loaded yet when the first proc address is
// asked for, dlsym returns null, and then *every* proc address this hands
// back is null -- the application takes the crash in its own code with no
// frame of ours on the stack. Exactly that happened to memsteer.so, which
// unlike libbg3le.so does not link the loader, so nothing had pulled it in
// when the engine made its first call.
//
// So the loader is asked for directly when RTLD_NEXT misses. NOLOAD first,
// because if it is already mapped that is the copy the application is using;
// a plain dlopen only as a last resort.
void* loader() {
    static void* handle = [] () -> void* {
        void* h = ::dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (h == nullptr) h = ::dlopen("libvulkan.so.1", RTLD_LAZY);
        if (h == nullptr) h = ::dlopen("libvulkan.so", RTLD_LAZY);
        return h;
    }();
    return handle;
}

template <typename Fn>
Fn real(const char* name) {
    if (void* next = ::dlsym(RTLD_NEXT, name)) {
        return reinterpret_cast<Fn>(next);
    }
    if (void* h = loader()) {
        if (void* found = ::dlsym(h, name)) {
            return reinterpret_cast<Fn>(found);
        }
    }
    logf("could not find %s in the Vulkan loader; leaving memory alone", name);
    return nullptr;
}

double now_s() {
    timespec t{};
    ::clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

// ---- the measurement ----

constexpr VkDeviceSize kProbeBytes = 8u << 20;   // 8 MiB
constexpr int kProbeReps = 3;

// Writes into one memory type, in GB/s, or 0 if it could not be measured.
double probe_type(VkDevice dev, std::uint32_t type, void const* src) {
    auto alloc = real<PFN_vkAllocateMemory>("vkAllocateMemory");
    auto freem = real<PFN_vkFreeMemory>("vkFreeMemory");
    auto map = real<PFN_vkMapMemory>("vkMapMemory");
    auto unmap = real<PFN_vkUnmapMemory>("vkUnmapMemory");
    if (!alloc || !freem || !map || !unmap) return 0.0;

    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = kProbeBytes;
    ai.memoryTypeIndex = type;

    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (alloc(dev, &ai, nullptr, &mem) != VK_SUCCESS) return 0.0;

    void* dst = nullptr;
    if (map(dev, mem, 0, kProbeBytes, 0, &dst) != VK_SUCCESS || dst == nullptr) {
        freem(dev, mem, nullptr);
        return 0.0;
    }

    std::memcpy(dst, src, kProbeBytes);   // warm, and not timed
    double best = 0.0;
    for (int r = 0; r < kProbeReps; ++r) {
        const double t0 = now_s();
        std::memcpy(dst, src, kProbeBytes);
        const double dt = now_s() - t0;
        if (dt > 0) {
            const double gbs = (double)kProbeBytes / dt / 1e9;
            if (gbs > best) best = gbs;
        }
    }

    unmap(dev, mem);
    freem(dev, mem, nullptr);
    return best;
}

// True if the device-local host-visible heaps are enough slower than plain
// host memory to be worth avoiding. Creates its own short-lived device so the
// answer is available before the engine has made one.
bool measure_should_steer(VkPhysicalDevice phys,
                          VkPhysicalDeviceMemoryProperties const& props) {
    auto createDev = real<PFN_vkCreateDevice>("vkCreateDevice");
    auto destroyDev = real<PFN_vkDestroyDevice>("vkDestroyDevice");
    if (!createDev || !destroyDev) return false;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo q{};
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = 0;
    q.queueCount = 1;
    q.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &q;

    VkDevice dev = VK_NULL_HANDLE;
    if (createDev(phys, &dci, nullptr, &dev) != VK_SUCCESS) {
        logf("vkmem: could not create a probe device; leaving memory alone");
        return false;
    }

    void* src = std::malloc(kProbeBytes);
    if (src == nullptr) {
        destroyDev(dev, nullptr);
        return false;
    }
    std::memset(src, 0xA5, kProbeBytes);

    double bestHost = 0.0;      // host-visible, not device-local
    double bestBar = 0.0;       // device-local and host-visible
    for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = props.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        const double gbs = probe_type(dev, i, src);
        if (gbs <= 0.0) continue;
        const bool deviceLocal = (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
        logf("vkmem: type %u writes at %.2f GB/s%s", i, gbs,
             deviceLocal ? " (device-local)" : "");
        double& slot = deviceLocal ? bestBar : bestHost;
        if (gbs > slot) slot = gbs;
    }

    std::free(src);
    destroyDev(dev, nullptr);

    if (bestHost <= 0.0 || bestBar <= 0.0) {
        logf("vkmem: incomplete measurement (host %.2f, device-local %.2f); "
             "leaving memory alone", bestHost, bestBar);
        return false;
    }

    // A quarter is a wide margin. A desktop's BAR writes land within a factor
    // of two of host memory, where steering would cost more than it saves;
    // this machine's differ by seventy.
    const bool steer = bestBar < bestHost * 0.25;
    logf("vkmem: host %.2f GB/s, device-local host-visible %.2f GB/s -- %s",
         bestHost, bestBar,
         steer ? "steering uploads to host memory"
               : "device-local writes are fast enough, leaving memory alone");
    return steer;
}

// ---- policy ----

std::mutex& lock() {
    static std::mutex m;
    return m;
}

bool decided = false;
bool steering = false;

bool should_steer(VkPhysicalDevice phys,
                 VkPhysicalDeviceMemoryProperties const& props) {
    std::lock_guard<std::mutex> guard(lock());
    if (decided) return steering;
    decided = true;

    switch (mode()) {
        case Mode::Off:
            logf("disabled by MEMSTEER=off");
            steering = false;
            break;
        case Mode::Force:
            logf("forced on by MEMSTEER=on");
            steering = true;
            break;
        case Mode::Measure:
            steering = measure_should_steer(phys, props);
            break;
    }
    return steering;
}

// Removes HOST_VISIBLE from the device-local types, but only while a
// host-only type remains for the engine to fall back to. Without that check
// this would leave it with nowhere to put uploads at all.
void hide_host_visible(VkPhysicalDeviceMemoryProperties* props) {
    bool haveHostOnly = false;
    for (std::uint32_t i = 0; i < props->memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags f = props->memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            && !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            haveHostOnly = true;
            break;
        }
    }
    if (!haveHostOnly) {
        logf("vkmem: no host-only memory type to fall back to; leaving the "
             "properties alone");
        return;
    }

    int hidden = 0;
    for (std::uint32_t i = 0; i < props->memoryTypeCount; ++i) {
        VkMemoryPropertyFlags& f = props->memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            f &= ~(VkMemoryPropertyFlags)VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            ++hidden;
        }
    }
    static bool said = false;
    if (!said) {
        said = true;
        logf("vkmem: hid HOST_VISIBLE from %d device-local memory types",
             hidden);
    }
}

void handle(VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties* props) {
    if (mode() == Mode::Off || props == nullptr) return;
    if (!should_steer(phys, *props)) return;
    hide_host_visible(props);
}

}  // namespace

extern "C" void vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties* props) {
    static const auto next =
        real<PFN_vkGetPhysicalDeviceMemoryProperties>(
            "vkGetPhysicalDeviceMemoryProperties");
    if (next == nullptr) return;
    next(phys, props);
    handle(phys, props);
}

extern "C" void vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice phys, VkPhysicalDeviceMemoryProperties2* props) {
    static const auto next =
        real<PFN_vkGetPhysicalDeviceMemoryProperties2>(
            "vkGetPhysicalDeviceMemoryProperties2");
    if (next == nullptr) return;
    next(phys, props);
    if (props != nullptr) handle(phys, &props->memoryProperties);
}

// The dispatch the engine actually uses. Everything above is only reachable
// because these hand back our pointers.
extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice dev,
                                                  const char* name);

namespace {

// Provided by src/vulkan_forward.cpp in libbg3le.so, for the entry points
// bg3se's ImGui overlay wraps. Weak because this file is also built into
// memsteer.so, which has no overlay.
extern "C" void* bg3le_vulkan_forwarder(char const* name)
    __attribute__((weak));

PFN_vkVoidFunction diverted(const char* name) {
    if (name == nullptr) return nullptr;

    // The overlay's forwarders first, and not gated on the steering mode:
    // the two features are independent, and BG3LE_VKMEM=off must not turn
    // the overlay off with it.
    if (bg3le_vulkan_forwarder != nullptr) {
        if (void* fn = bg3le_vulkan_forwarder(name)) {
            return (PFN_vkVoidFunction)fn;
        }
    }

    if (mode() == Mode::Off) return nullptr;
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties") == 0) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties;
    }
    if (std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2") == 0
        || std::strcmp(name, "vkGetPhysicalDeviceMemoryProperties2KHR") == 0) {
        return (PFN_vkVoidFunction)vkGetPhysicalDeviceMemoryProperties2;
    }
    return nullptr;
}

}  // namespace

extern "C" PFN_vkVoidFunction vkGetDeviceProcAddr(VkDevice dev,
                                                  const char* name) {
    static const auto next =
        real<PFN_vkGetDeviceProcAddr>("vkGetDeviceProcAddr");
    if (auto ours = diverted(name)) return ours;
    return next != nullptr ? next(dev, name) : nullptr;
}

extern "C" PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance inst,
                                                    const char* name) {
    static const auto next =
        real<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    // Ours whenever either feature is on: the device-level lookup is how both
    // the steering and the overlay's forwarders are reached.
    const bool wanted =
        mode() != Mode::Off
        || (bg3le_vulkan_forwarder != nullptr
            && bg3le_vulkan_forwarder("vkQueuePresentKHR") != nullptr);
    if (name != nullptr && wanted
        && std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
        return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    }
    if (auto ours = diverted(name)) return ours;
    return next != nullptr ? next(inst, name) : nullptr;
}
