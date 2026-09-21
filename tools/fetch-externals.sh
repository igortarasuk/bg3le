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
# slot patching and call-site patching. protobuf, SDL2 and oneTBB come from
# the distro.
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
    bsdtar -xf noesis.zip -C Noesis
    rm noesis.zip
fi

echo "== header-only / portable sources =="
clone glm       https://github.com/g-truc/glm 1.0.3
clone imgui     https://github.com/Norbyte/imgui
clone lua       https://github.com/Norbyte/lua-dos
clone rapidjson https://github.com/tencent/rapidjson
clone tinycrypt https://github.com/intel/tinycrypt
clone Vulkan    https://github.com/KhronosGroup/Vulkan-Headers vulkan-sdk-1.4.357

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
