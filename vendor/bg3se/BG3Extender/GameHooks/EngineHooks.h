#pragma once

#include <CoreLib/Wrappers.h>
#include <GameHooks/EngineHooksFwdDecl.h>

namespace bg3se
{
    class EngineHooks
    {
    public:
#define HOOK_DEFN(name, sym, defn) enum class name##Tag {}; \
    WrappableFunction<name##Tag, defn> name;
#include <GameHooks/EngineHooks.inl>
#undef HOOK_DEFN

        void HookAll();
        void UnhookAll();
    };
}
