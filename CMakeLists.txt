cmake_minimum_required(VERSION 3.16)

# clang is required, not merely supported: the vendored bg3se sources need
# -fdeclspec, -fms-extensions and -fdelayed-template-parsing, none of which gcc
# has. Set before project() so the toolchain is chosen correctly.
if(NOT DEFINED CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER clang)
endif()
if(NOT DEFINED CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER clang++)
endif()

project(bg3le C CXX)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR
        "bg3le requires clang; ${CMAKE_CXX_COMPILER_ID} cannot compile the "
        "vendored bg3se sources (-fdeclspec, -fms-extensions, "
        "-fdelayed-template-parsing)")
endif()

# Default to Release. Without NDEBUG the vendored sources compile their debug
# branches, which pull in more Windows API surface and enable upstream asserts.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# libc++ is not a preference. The native game is built against it, so its
# std::string is 24 bytes; libstdc++ would give 32 and silently shift every
# field after a string in a component. Everything in one library has to agree
# on a standard library, so this applies to bg3le's own sources too.
#
# Linked statically, for the same reason Lua is vendored: an LD_PRELOAD shim
# loaded inside the Steam runtime container cannot rely on host libraries.
add_compile_options(-stdlib=libc++)
# Link the static standard library explicitly rather than via
# -static-libstdc++. clang appends its own -lc++ last, which puts it after
# libc++abi and leaves the __cxa_* symbols unresolved; naming both archives
# ourselves keeps them in dependency order (libc++ needs libc++abi).
add_link_options(-stdlib=libc++ -nostdlib++ -static-libgcc)

find_library(LIBCXX_STATIC NAMES libc++.a REQUIRED)
find_library(LIBCXXABI_STATIC NAMES libc++abi.a REQUIRED)
link_libraries(${LIBCXX_STATIC} ${LIBCXXABI_STATIC})

# ---------------------------------------------------------------------------
# Lua (Norbyte's fork; see external/lua/README.bg3le)
# ---------------------------------------------------------------------------
file(GLOB LUA_SOURCES external/lua/*.c)
# bg3se builds this fork as C++ (LuaLib.vcxproj sets CompileAsCpp), and its
# own headers rely on that: lua.h names CMetatable and TString without a
# struct tag, which is only legal in C++.
set_source_files_properties(${LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(lua_static STATIC ${LUA_SOURCES})
target_include_directories(lua_static PUBLIC external/lua)
target_compile_definitions(lua_static PRIVATE LUA_USE_LINUX)
# ltable.c uses uintptr_t, which MSVC's headers supply transitively and
# libc++ does not.
target_compile_options(lua_static PRIVATE -include cstdint)
set_target_properties(lua_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# ImGui (Norbyte's fork), for the extender's in-game UI. Only the Vulkan
# backend: the native Linux build renders through Vulkan, and the D3D11 one
# needs the DirectX headers.
# ---------------------------------------------------------------------------
set(EXTERNAL ${CMAKE_CURRENT_SOURCE_DIR}/external/third_party)

add_library(imgui STATIC
    ${EXTERNAL}/imgui/imgui.cpp
    ${EXTERNAL}/imgui/imgui_draw.cpp
    ${EXTERNAL}/imgui/imgui_tables.cpp
    ${EXTERNAL}/imgui/imgui_widgets.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_vulkan.cpp
    ${EXTERNAL}/imgui/backends/imgui_impl_sdl2.cpp
)
target_include_directories(imgui PUBLIC ${EXTERNAL}/imgui
    ${EXTERNAL}/Vulkan/include /usr/include/SDL2)
target_compile_options(imgui PRIVATE -w)
set_target_properties(imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# Vendored bg3se
#
# See vendor/NOTICE.md for attribution and for every change made to build this
# with clang. tools/vendor-exclude.txt records the translation units bg3le
# deliberately does not build, and why.
# ---------------------------------------------------------------------------
set(VENDOR_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/vendor/bg3se)
set(NOESIS ${EXTERNAL}/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include)

if(NOT EXISTS ${NOESIS})
    message(FATAL_ERROR "externals missing; run tools/fetch-externals.sh")
endif()

file(GLOB_RECURSE VENDOR_SOURCES ${VENDOR_ROOT}/*.cpp ${VENDOR_ROOT}/*.cc)

# Upstream compiles one .inl as a translation unit -- BG3Extender.vcxproj lists
# it under ClCompile and nothing includes it. CMake will not generate a rule
# for an .inl, so src/vendor/ wraps it.
list(APPEND VENDOR_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor/stats_object_tu.cpp)

# Kept in step with tools/vendor-exclude.txt.
set(VENDOR_EXCLUDE
    BG3Extender/Extender/Shared/dllmain.cpp        # DLL entry; we use LD_PRELOAD
    BG3Extender/Extender/Shared/CrashReporter.cpp  # Windows crash reporting
    BG3Extender/Extender/Shared/DWriteWrapper.cpp  # DWrite proxy-DLL trick
    BG3Extender/GameHooks/DataLibraries.cpp        # PE scanner; we read ELF
    BG3Extender/GameHooks/DataLibrariesBG3Game.cpp
    BG3Extender/GameHooks/OsirisWrappers.cpp       # Detours; we interpose PLT
    BG3Extender/GameHooks/EngineHooks.cpp
    CoreLib/SymbolMapper.cpp
    BG3Extender/Extender/Client/SDL.cpp
    CoreLib/Console.cpp                            # Windows console internals
    # Byte-identical duplicate of CoreLib/tinyxml2.cpp; building both would
    # define every symbol twice.
    BG3Extender/Extender/Shared/tinyxml2.cpp
)
foreach(rel ${VENDOR_EXCLUDE})
    list(REMOVE_ITEM VENDOR_SOURCES ${VENDOR_ROOT}/${rel})
endforeach()

add_library(bg3se_vendor STATIC ${VENDOR_SOURCES})
set_target_properties(bg3se_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_include_directories(bg3se_vendor PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/compat
    ${VENDOR_ROOT}
    ${VENDOR_ROOT}/BG3Extender
    ${NOESIS}
    ${EXTERNAL}/glm
    ${EXTERNAL}/imgui
    ${EXTERNAL}/rapidjson/include
    ${EXTERNAL}/optick/src
    ${EXTERNAL}/tinycrypt/lib/include
    /usr/include/SDL2
)

target_compile_definitions(bg3se_vendor PUBLIC
    OSI_EOCAPP
    OSI_EXTENSION_BUILD
    NS_STATIC_LIBRARY
    GLM_FORCE_INTRINSICS   # MSVC enables SIMD implicitly; glm needs it set
    GOOGLE_PROTOBUF_NO_RTTI
    _ITERATOR_DEBUG_LEVEL=0
    BG3LE_NO_DX11   # the D3D11 ImGui backend needs the DirectX headers
)

# -fdeclspec/-fms-extensions: __declspec and MSVC struct extensions.
# -fdelayed-template-parsing: MSVC resolves dependent names at instantiation.
# Not -fms-compatibility: it de-keywords char16_t/char32_t and breaks libc++.
# MSVC enables SSE4.2 and POPCNT by default when targeting x64, so upstream
# uses _mm_popcnt_u64 and rapidjson's SSE4.2 scanner without asking for them.
target_compile_options(bg3se_vendor PUBLIC
    -msse4.2 -mpopcnt
    -fdeclspec -fms-extensions -fdelayed-template-parsing
    -Wno-delayed-template-parsing-in-cxx20
    -include ${CMAKE_CURRENT_SOURCE_DIR}/vendor/compat/msvc_compat.h
)
# Upstream warnings are not ours to fix, and there are some 1800 of them.
target_compile_options(bg3se_vendor PRIVATE -w)

target_link_libraries(bg3se_vendor PUBLIC lua_static imgui protobuf-lite tbb)

# ---------------------------------------------------------------------------
# bg3le
# ---------------------------------------------------------------------------
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
target_link_options(bg3le PRIVATE -Wl,--no-undefined)
target_compile_options(bg3le PRIVATE -Wall -Wextra -fvisibility=default)

add_executable(symdump tools/symdump.cpp src/elf_symbols.cpp src/log.cpp)
