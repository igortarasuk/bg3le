#pragma once

// bg3le: where a delegate's arguments go, and where Push finds its
// function. Declared rather than included, because the queue is bg3le's own
// and does not depend on anything here.
namespace bg3se::lua { struct ImguiHandle; }

namespace bg3le {
    using Widget = bg3se::lua::ImguiHandle const&;

    void delegate_push(lua_State* L, uint32_t id);
    void delegate_post(uint32_t id);
    void delegate_post(uint32_t id, Widget widget);
    void delegate_post(uint32_t id, Widget widget, bool value);
    void delegate_post(uint32_t id, Widget widget, int value);
    void delegate_post(uint32_t id, Widget widget, Widget value);
    void delegate_post(uint32_t id, Widget widget, glm::vec4 const& value);
    void delegate_post(uint32_t id, Widget widget, glm::ivec4 const& value);
    void delegate_post(uint32_t id, Widget widget,
                       bg3se::STDString const& value);

    // Anything bg3le has no delivery for. Non-template overloads win, so
    // this catches only the shapes above have not covered; it reports the
    // drop rather than failing to build.
    void delegate_unsupported(uint32_t id, char const* what);

    template <class... TArgs>
    inline void delegate_post(uint32_t id, TArgs&&...)
    {
        delegate_unsupported(id, "an argument shape bg3le does not deliver");
    }
}

BEGIN_NS(lua)

template <class TArgs, class TReturn>
struct ProtectedFunctionCaller;

template <class T>
class LuaDelegate;

// bg3le: a delegate here is an id into bg3le's own table, not a
// lua::RegistryEntry.
//
// Upstream's holds a RegistryEntry, which reaches its manager through
// lua::State::FromLua(L) -- bg3se's own Lua state. bg3le runs its own
// contexts and never starts bg3se's, so constructing one jumped through a
// null. And calling one marshals the arguments through bg3se's userdata
// machinery, whose metatables are registered during that same state's init,
// so a widget would arrive in Lua as an object with no methods.
//
// So the id is all that is stored, and Call posts the arguments to a queue
// bg3le drains on the thread that owns the context which registered the
// callback -- the render thread is where a widget fires, and it must not
// touch Lua. See src/vendor/imgui_events.cpp and Ext.IMGUI in
// src/lua_host.cpp.
//
// Zero is "not set", which is what operator bool reports, and is what the
// widget code checks before queueing anything at all.
template<class TRet, class ...TArgs>
class LuaDelegate<TRet(TArgs...)>
{
public:
    using Function = TRet(TArgs...);
    using ArgumentTuple = std::tuple<TArgs...>;

    inline LuaDelegate() {}

    // The three upstream constructors take a function off a Lua stack, which
    // only bg3se's own binding layer does. bg3le's callbacks are registered
    // through Ext._Internal.ImguiSetCallback instead, so these yield an
    // unset delegate rather than pretending to hold one.
    inline LuaDelegate(lua_State* L, int index) {}
    inline LuaDelegate(lua_State* L, Ref const& local) {}
    inline LuaDelegate(lua_State* L, FunctionRef const& f) {}

    inline ~LuaDelegate() {}

    inline LuaDelegate(LuaDelegate const& o)
        : id_(o.id_)
    {}

    inline LuaDelegate(LuaDelegate && o) noexcept
        : id_(o.id_)
    {}

    inline LuaDelegate& operator = (LuaDelegate const& o)
    {
        id_ = o.id_;
        return *this;
    }

    inline LuaDelegate& operator = (LuaDelegate && o) noexcept
    {
        id_ = o.id_;
        return *this;
    }

    explicit inline operator bool() const
    {
        return id_ != 0;
    }

    inline uint32_t Id() const
    {
        return id_;
    }

    inline void SetId(uint32_t id)
    {
        id_ = id;
    }

    inline void Push(lua_State* L) const
    {
        bg3le::delegate_push(L, id_);
    }

    TRet Call(lua_State* L, TArgs... args)
    {
        if constexpr (std::is_same_v<TRet, void>) {
            Call(L, std::tuple(args...));
        } else {
            return Call(L, std::tuple(args...));
        }
    }

    TRet Call(lua_State* L, ArgumentTuple const& args)
    {
        std::apply(
            [this](auto const&... unpacked) {
                bg3le::delegate_post(id_, unpacked...);
            },
            args);

        if constexpr (!std::is_same_v<TRet, void>) {
            return TRet{};
        }
    }

private:
    uint32_t id_{ 0 };
};

template <class T>
class DeferredLuaDelegateCall;

template <class TRet, class... TArgs>
class DeferredLuaDelegateCall<TRet (TArgs...)>
{
public:
    DeferredLuaDelegateCall(LuaDelegate<TRet (TArgs...)> delegate, TArgs... args)
        : delegate_(delegate), args_(std::tuple(args...))
    {}

    TRet Call(lua_State* L)
    {
        if constexpr (std::is_same_v<TRet, void>) {
            delegate_.Call(L, args_);
        } else {
            return delegate_.Call(L, args_);
        }
    }

private:
    LuaDelegate<TRet(TArgs...)> delegate_;
    std::tuple<TArgs...> args_;
};

class GenericDeferredLuaDelegateCall
{
public:
    virtual ~GenericDeferredLuaDelegateCall() {}
    virtual void Call(lua_State* L) = 0;
};

template <class TRet, class... TArgs>
class DeferredLuaDelegateCallImpl : public GenericDeferredLuaDelegateCall
{
public:
    DeferredLuaDelegateCallImpl(LuaDelegate<TRet(TArgs...)> const& delegate, TArgs... args)
        : call_(delegate, args...)
    {}

    ~DeferredLuaDelegateCallImpl() override {}

    void Call(lua_State* L) override
    {
        call_.Call(L);
    }

private:
    DeferredLuaDelegateCall<TRet(TArgs...)> call_;
};

class DeferredLuaDelegateQueue
{
public:
    inline DeferredLuaDelegateQueue(char const* name)
#if USE_OPTICK
        : description_(OPTICK_DESC(name, "", 0, IO))
#endif
    {}

    template <class TRet, class... TArgs>
    void Call(LuaDelegate<TRet(TArgs...)> const& delegate, TArgs... args)
    {
        if (!delegate) return;

        auto call = GameAlloc<DeferredLuaDelegateCallImpl<TRet, TArgs...>>(delegate, std::forward<TArgs>(args)...);
        queue_.push_back(call);
    }
    
    template <class TRet, class... TArgs>
    void Call(LuaDelegate<TRet(TArgs...)> && delegate, TArgs... args)
    {
        if (!delegate) return;

        auto call = GameAlloc<DeferredLuaDelegateCallImpl<TRet, TArgs...>>(std::move(delegate), std::forward<TArgs>(args)...);
        queue_.push_back(call);
    }

    void Flush(lua_State* L);
    void Flush();

private:
    Array<GenericDeferredLuaDelegateCall*> queue_;
#if USE_OPTICK
    ::Optick::EventDescription* description_;
#endif
};

END_NS()
