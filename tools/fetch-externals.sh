#!/bin/bash
#
# Fetches the third-party dependencies that vendor/bg3se needs, into
# external/third_party/. Kept separate from external/lua, which is bg3le's own
# vendored Lua 5.4.9 and is tracked in git.
# Derived from External/pull-externals.bat in the BG3 Script Extender by
# Norbyte and the bg3se contributors (https://github.com/Norbyte/bg3se),
# MIT + Commons Clause. With thanks to them.
#
# Detours is deliberately absent: bg3le hooks via PLT interposition, vtable
# slot patching and call-site patching. SDL2 and Vulkan come from the Steam
# runtime container at run time. oneTBB is deliberately not used at all --
# vendor/compat implements the two concurrent containers bg3se needs over the
# standard library, because libtbb is not in that container.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
EXT="$ROOT/external/third_party"
mkdir -p "$EXT"
cd "$EXT"

clone() {  # clone <dir> <url> [branch]
    if [ -d "$1" ]; then echo "  $1: present"; return; fi
    if [ -n "${3:-}" ]; then git clone --depth 1 --branch "$3" "$2" "$1"
    else git clone --depth 1 "$2" "$1"; fi
}

echo "== Noesis (the Windows SDK ships portable C++ headers) =="
if [ -d Noesis ]; then
    echo "  Noesis: present"
else
    curl -fL http://bg3se-updates.norbyte.dev/Stuff/NoesisGUI-NativeSDK-win-3.1.7-Indie.zip \
         -o noesis.zip
    mkdir -p Noesis
    # GNU tar cannot read zip archives; on Windows tar is bsdtar, which can.
    # Debian's libarchive-tools package (bsdtar) has had unresolvable
    # dependency pins on some mirrors, so fall back to unzip, which every
    # Debian-based build image carries without friction.
    if command -v bsdtar >/dev/null 2>&1; then
        bsdtar -xf noesis.zip -C Noesis
    else
        unzip -q noesis.zip -d Noesis
    fi
    rm noesis.zip
fi

echo "== header-only / portable sources =="
clone glm       https://github.com/g-truc/glm 1.0.3
clone imgui     https://github.com/Norbyte/imgui
clone lua       https://github.com/Norbyte/lua-dos
clone rapidjson https://github.com/tencent/rapidjson
clone tinycrypt https://github.com/intel/tinycrypt
clone Vulkan    https://github.com/KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.357
clone optick    https://github.com/Norbyte/optick

echo "== abseil (built from source against libc++) =="
# protobuf needs abseil, and 45 of the symbols it calls take or return
# std::string. The distribution abseil is libstdc++, so it has to be rebuilt
# too or the mismatch just moves one layer down. The version has to match
# whatever protobuf expects.
ABSL_PREFIX="$EXT/abseil/install"
if [ ! -f "$ABSL_PREFIX/lib/libabsl_strings.a" ]; then
    if [ ! -d abseil ]; then
        git clone --depth 1 --branch 20260817.0 \
            https://github.com/abseil/abseil-cpp abseil
    fi
    cmake -S abseil -B abseil/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -DABSL_PROPAGATE_CXX_STD=ON \
        -DABSL_ENABLE_INSTALL=ON \
        -DBUILD_TESTING=OFF \
        -DCMAKE_INSTALL_PREFIX="$ABSL_PREFIX"
    cmake --build abseil/build
    cmake --install abseil/build
else
    echo "  abseil: present"
fi

echo "== protobuf (built from source against libc++) =="
# The distribution package is built against libstdc++ and exports
# std::__cxx11 symbols. bg3se's generated message code is part of the extender
# core, not just its networking, so it has to link a protobuf whose
# std::string matches ours -- mixing the two ABIs in one process is not
# something that fails loudly at runtime. Upstream builds protobuf from source
# on Windows for the same reason.
if [ -d protobuf/build/libprotobuf-lite.a ] || [ -f protobuf/build/libprotobuf-lite.a ]; then
    echo "  protobuf: present"
else
    if [ ! -d protobuf ]; then
        git clone --depth 1 --branch v36.1 --recurse-submodules --shallow-submodules \
            https://github.com/protocolbuffers/protobuf protobuf
    fi
    cmake -S protobuf -B protobuf/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_COMPILER=clang \
        -DCMAKE_CXX_FLAGS="-stdlib=libc++" \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DCMAKE_CXX_STANDARD=17 \
        -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_PROTOC_BINARIES=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=OFF \
        -Dprotobuf_ABSL_PROVIDER=package \
        -DCMAKE_PREFIX_PATH="$ABSL_PREFIX"
    cmake --build protobuf/build --target libprotobuf-lite
fi

echo "== patch Noesis for clang =="
# NsCore/TypePropertyImpl.h marks void Get(const void*, void*) const as
# override, but TypeProperty declares GetCopy and no such overload, so it never
# overrode anything. clang rejects it; dropping the keyword changes no
# dispatch. Noesis is third-party and not vendored, so this runs at fetch time.
NS="Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include/NsCore/TypePropertyImpl.h"
if grep -q "void Get(const void\* ptr, void\* dest) const override;" "$NS"; then
    sed -i "s|void Get(const void\* ptr, void\* dest) const override;|void Get(const void* ptr, void* dest) const;|" "$NS"
    echo "  patched TypePropertyImpl.h"
else
    echo "  TypePropertyImpl.h: already patched"
fi

echo "done"
