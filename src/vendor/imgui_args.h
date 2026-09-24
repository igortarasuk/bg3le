#pragma once

// One argument or result of an Ext.IMGUI method call.
//
// The widget methods take about a dozen distinct signatures between them,
// drawn from the same handful of types, so they are dispatched by name with
// a tagged argument array rather than one entry point each. Lua marshals
// into this and reads results back out of it; src/vendor/imgui_methods.cpp
// turns it into the arguments upstream declared.
//
// An argument of kind None is one the caller did not pass, which is what
// upstream's std::optional means.

#include <cstddef>
#include <cstdint>

extern "C" {

enum ImguiArgKind : std::uint8_t {
    kImguiArgNone = 0,
    kImguiArgBool,
    kImguiArgInt,
    kImguiArgNumber,
    kImguiArgText,
    kImguiArgVec2,
    kImguiArgVec3,
    kImguiArgVec4,
    kImguiArgHandle,
};

struct ImguiArg {
    std::uint8_t Kind;
    bool Bool;
    int Int;
    double Number;
    char const* Text;
    float Vec[4];
    std::uint64_t Handle;
};

// At most this many arguments to one method; the widest upstream method takes
// five.
constexpr std::size_t kImguiMaxArgs = 8;

}  // extern "C"
