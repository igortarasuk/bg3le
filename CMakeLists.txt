cmake_minimum_required(VERSION 3.16)
project(bg3le C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Lua is vendored and statically linked: an LD_PRELOAD shim running inside the
# Steam runtime container cannot rely on host libraries being present.
file(GLOB LUA_SOURCES external/lua/*.c)
# bg3se builds this fork as C++ (LuaLib.vcxproj sets CompileAsCpp), and its
# own headers rely on that: lua.h names CMetatable and TString without a
# struct tag, which is only legal in C++.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC external/lua)
target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
# ltable.c uses uintptr_t, which MSVC's headers supply transitively and
# libstdc++/libc++ do not.
target_compile_options(lua_static PRIVATE -include cstdint)
set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(bg3le SHARED
    src/preload.cpp
    src/elf_symbols.cpp
    src/log.cpp
    src/mem.cpp
    src/lua_host.cpp
    src/osi.cpp
    src/debug_server.cpp
    src/hook.cpp
    src/console.cpp
    src/stackdump.cpp
    src/fast_alloc.cpp
    src/win_compat.cpp
)
target_link_libraries(bg3le PRIVATE dl lua_static)
target_link_options(bg3le PRIVATE -static-libstdc++ -static-libgcc
    -Wl,--no-undefined)
target_compile_options(bg3le PRIVATE -Wall -Wextra -fvisibility=default)

add_executable(symdump tools/symdump.cpp src/elf_symbols.cpp src/log.cpp)
