// Where a widget's callbacks go.
//
// A widget fires on the render thread, inside IMGUIManager::Update, and that
// thread must not touch a Lua state -- bg3le's contexts are driven from the
// game's own tick threads. So the whole path is: the widget's delegate holds
// an id (see vendor/.../LuaDelegate.h, which bg3le replaced for this), the
// arguments are copied into the queue below, and Ext.IMGUI drains the queue
// from the tick of the context that registered the callback.
//
// Nothing here holds a Lua reference. The function itself stays in a table on
// the Lua side, keyed by the same id, so a callback registered by the client
// context is never called on the server's state and an id that outlives its
// widget simply finds nothing.

#include <stdafx.h>

#include <Extender/Client/IMGUI/Objects.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include "../log.h"

extern "C" void const* bg3le_meta_class(char const* className);
extern "C" bool bg3le_meta_field(void const* handle, char const* name,
                                 std::uint32_t* offset, std::uint16_t* size,
                                 std::uint8_t* kind, std::uint8_t* elemKind,
                                 std::uint16_t* elemCount);

namespace bg3le {

// The widget the handle names, and the class its properties are described
// by. src/vendor/imgui_api.cpp owns the object manager.
bg3se::extui::Renderable* imgui_renderable(std::uint64_t handle);
char const* imgui_type_name(std::uint64_t handle);

namespace {

// What a callback's second argument can be. Every widget event passes the
// widget first and at most one more value; see the LuaDelegate signatures in
// Extender/Client/IMGUI/Objects.h.
enum class ArgKind : std::uint8_t {
    None,
    Bool,
    Int,
    Widget,
    Vec4,
    IVec4,
    String,
};

struct Event {
    std::uint32_t Id{0};
    void* State{nullptr};
    std::uint64_t Widget{0};
    ArgKind Kind{ArgKind::None};
    bool Bool{false};
    int Int{0};
    std::uint64_t Other{0};
    float Vec[4]{};
    int IVec[4]{};
    std::string Str;
};

std::mutex& lock() {
    static std::mutex m;
    return m;
}

// Which context registered each id, so an event is only ever handed to the
// state that can call it.
std::unordered_map<std::uint32_t, void*>& owners() {
    static std::unordered_map<std::uint32_t, void*> map;
    return map;
}

std::deque<Event>& queue() {
    static std::deque<Event> q;
    return q;
}

// A click that nobody drains would otherwise grow without bound -- the
// overlay runs whether or not a mod is listening.
constexpr std::size_t kMaxQueued = 4096;

bool start_event(std::uint32_t id, Event* out) {
    if (id == 0) return false;

    auto const owner = owners().find(id);
    if (owner == owners().end()) return false;

    out->Id = id;
    out->State = owner->second;
    return true;
}

void finish_event(Event&& event) {
    if (queue().size() >= kMaxQueued) {
        static bool said = false;
        if (!said) {
            said = true;
            logf("imgui: %zu callbacks are queued and undelivered; dropping "
                 "the oldest. Is the context that registered them ticking?",
                 queue().size());
        }
        queue().pop_front();
    }
    queue().push_back(std::move(event));
}

}  // namespace

// ---- what the widgets call, through LuaDelegate::Call -----------------------

void delegate_post(std::uint32_t id) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   bool value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::Bool;
    event.Bool = value;
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   int value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::Int;
    event.Int = value;
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   bg3se::lua::ImguiHandle const& value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::Widget;
    event.Other = value.Handle;
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   glm::vec4 const& value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::Vec4;
    for (int i = 0; i < 4; ++i) event.Vec[i] = value[i];
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   glm::ivec4 const& value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::IVec4;
    for (int i = 0; i < 4; ++i) event.IVec[i] = value[i];
    finish_event(std::move(event));
}

void delegate_post(std::uint32_t id, bg3se::lua::ImguiHandle const& widget,
                   bg3se::STDString const& value) {
    const std::lock_guard<std::mutex> held(lock());
    Event event;
    if (!start_event(id, &event)) return;
    event.Widget = widget.Handle;
    event.Kind = ArgKind::String;
    event.Str.assign(value.data(), value.size());
    finish_event(std::move(event));
}

void delegate_unsupported(std::uint32_t id, char const* what) {
    if (id == 0) return;
    static bool said = false;
    if (said) return;
    said = true;
    logf("imgui: a callback fired with %s; it was dropped", what);
}

// bg3se's property maps push a delegate back to Lua so a mod can read the
// handler it set. bg3le keeps the function on the Lua side instead, so
// there is nothing here to push -- Ext.IMGUI answers that read itself.
void delegate_push(lua_State* L, std::uint32_t id) {
    (void)id;
    lua_pushnil(L);
}

}  // namespace bg3le

// ---- what Ext.IMGUI calls ---------------------------------------------------

namespace bg3le {

namespace {

// Where the named event lives in the widget, or false if it has no such
// event. A delegate is a single id and nothing else, so anything of another
// width under this name is a different kind of property and writing an id
// into it would corrupt the widget.
bool delegate_slot(std::uint64_t handle, char const* name,
                   std::uint32_t** at) {
    auto* object = imgui_renderable(handle);
    if (object == nullptr || name == nullptr) return false;

    char const* shortName = imgui_type_name(handle);
    if (shortName == nullptr) return false;

    const std::string qualified = std::string("extui::") + shortName;
    void const* fields = bg3le_meta_class(qualified.c_str());
    if (fields == nullptr) return false;

    std::uint32_t offset = 0;
    std::uint16_t size = 0;
    std::uint8_t kind = 0;
    std::uint8_t elemKind = 0;
    std::uint16_t elemCount = 0;
    if (!bg3le_meta_field(fields, name, &offset, &size, &kind, &elemKind,
                          &elemCount)) {
        return false;
    }
    if (size != sizeof(std::uint32_t)) return false;

    *at = reinterpret_cast<std::uint32_t*>(
        reinterpret_cast<char*>(object) + offset);
    return true;
}

}  // namespace

}  // namespace bg3le

// Points the named event of a widget at a fresh callback id and hands the id
// back, so Lua can file the function under it. Zero if the widget has no such
// event.
//
// `state` identifies the calling context; only that context's drain is
// offered the resulting events.
extern "C" std::uint32_t bg3le_imgui_set_callback(std::uint64_t handle,
                                                  char const* name,
                                                  void* state) {
    std::uint32_t* slot = nullptr;
    if (!bg3le::delegate_slot(handle, name, &slot)) return 0;

    std::uint32_t id = 0;
    {
        const std::lock_guard<std::mutex> held(bg3le::lock());
        static std::uint32_t next = 0;
        if (++next == 0) ++next;  // zero means "not set"
        id = next;
        bg3le::owners()[id] = state;
    }

    // Last, so the widget cannot fire with an id that has no owner yet.
    *slot = id;
    return id;
}

// Clears the named event, so the widget stops queueing anything, and forgets
// the id.
extern "C" bool bg3le_imgui_clear_callback(std::uint64_t handle,
                                           char const* name) {
    std::uint32_t* slot = nullptr;
    if (!bg3le::delegate_slot(handle, name, &slot)) return false;

    const std::uint32_t was = *slot;
    *slot = 0;

    const std::lock_guard<std::mutex> held(bg3le::lock());
    bg3le::owners().erase(was);
    return true;
}

// The next queued callback for this context, or false if there is none.
//
// One at a time rather than a batch, because a handler can create and destroy
// widgets and the caller should see the queue as it is after that.
extern "C" bool bg3le_imgui_take_event(void* state, std::uint32_t* id,
                                       std::uint64_t* widget,
                                       std::uint8_t* argKind, bool* argBool,
                                       int* argInt, std::uint64_t* argWidget,
                                       float* argVec, int* argIVec,
                                       char const** argString) {
    const std::lock_guard<std::mutex> held(bg3le::lock());

    // Kept until the next take, which is as long as the caller needs it.
    static std::string text;

    for (auto it = bg3le::queue().begin(); it != bg3le::queue().end(); ++it) {
        if (it->State != state) continue;

        *id = it->Id;
        *widget = it->Widget;
        *argKind = (std::uint8_t)it->Kind;
        *argBool = it->Bool;
        *argInt = it->Int;
        *argWidget = it->Other;
        for (int i = 0; i < 4; ++i) {
            argVec[i] = it->Vec[i];
            argIVec[i] = it->IVec[i];
        }
        text = std::move(it->Str);
        *argString = text.c_str();

        bg3le::queue().erase(it);
        return true;
    }
    return false;
}
