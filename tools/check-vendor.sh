#!/bin/bash
#
# Compiles the vendored bg3se sources and reports what fails. Run
# tools/fetch-externals.sh first.
#
# The code under test is by Norbyte and the bg3se contributors
# (https://github.com/Norbyte/bg3se), MIT + Commons Clause. See vendor/NOTICE.md
# for what was changed to build it with clang. With thanks to them.
#
# With no argument, compiles the module registration TU, which reaches almost
# everything. Pass a path to compile some other file instead.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)/.."
ROOT="$(cd "$ROOT" && pwd)"
V="$ROOT/vendor/bg3se"
E="$ROOT/external/third_party"
N="$E/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include"

if [ ! -d "$N" ]; then
    echo "externals missing; run tools/fetch-externals.sh" >&2
    exit 1
fi

TU="${1:-$V/BG3Extender/Lua/Libs/LuaSharedLibs.cpp}"

# -fdeclspec/-fms-extensions: __declspec and MSVC struct extensions.
# -fdelayed-template-parsing: MSVC resolves dependent names at instantiation.
# GLM_FORCE_INTRINSICS: MSVC enables SIMD implicitly; without it glm turns off
#   aligned gentypes and its own headers stop compiling.
# libc++ is required, not optional: the native game is built against it, so
#   std::string is 24 bytes there as here, rather than the 32 MSVC uses.
# Not -fms-compatibility: it de-keywords char16_t/char32_t and breaks libc++.
clang++ -std=gnu++23 -stdlib=libc++ -fsyntax-only \
    -fdeclspec -fms-extensions -fdelayed-template-parsing \
    -Wno-delayed-template-parsing-in-cxx20 -ferror-limit=60 \
    -DOSI_EOCAPP -DOSI_EXTENSION_BUILD -DNS_STATIC_LIBRARY -DNDEBUG \
    -DGLM_FORCE_INTRINSICS -DGOOGLE_PROTOBUF_NO_RTTI -D_ITERATOR_DEBUG_LEVEL=0 \
    -include "$ROOT/vendor/compat/msvc_compat.h" \
    -I"$ROOT/vendor/compat" -I"$V" -I"$V/BG3Extender" -I"$N" \
    -I"$E/glm" -I"$E/imgui" -I"$E/rapidjson/include" -I"$ROOT/external/lua" \
    -I"$E/optick/src" -I/usr/include/SDL2 \
    "$TU"
rc=$?

if [ $rc -eq 0 ]; then echo "$(basename "$TU"): compiles clean"; fi
exit $rc
