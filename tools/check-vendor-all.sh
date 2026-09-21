#!/bin/bash
#
# Syntax-checks every vendored bg3se TU and prints an error count per file, so
# progress porting the tree is visible at a glance. Run tools/fetch-externals.sh
# first.
#
# Note that the total is a poor metric: a fatal error stops a TU early and
# hides everything after it, so clearing one fatal usually makes the total go
# up. Count the files at zero instead.
B=/home/lenon/bg3mods/bg3le
V=$B/vendor/bg3se
E=$B/external/third_party
N=$E/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include
OUT=${TMPDIR:-/tmp}/bg3le-vendor-check
mkdir -p "$OUT"

one() {
    f="$1"
    B=/home/lenon/bg3mods/bg3le
    V=$B/vendor/bg3se
    E=$B/external/third_party
    N=$E/Noesis/NoesisGUI-NativeSDK-win-3.1.7-Indie/Include
    OUT=${TMPDIR:-/tmp}/bg3le-vendor-check
    name=$(echo "$f" | tr '/' '_')
    clang++ -std=gnu++23 -stdlib=libc++ -fsyntax-only \
        -fdeclspec -fms-extensions -fdelayed-template-parsing \
        -Wno-delayed-template-parsing-in-cxx20 -ferror-limit=200 -w \
        -DOSI_EOCAPP -DOSI_EXTENSION_BUILD -DNS_STATIC_LIBRARY -DNDEBUG \
        -DGLM_FORCE_INTRINSICS -DGOOGLE_PROTOBUF_NO_RTTI -D_ITERATOR_DEBUG_LEVEL=0 \
        -include "$B/vendor/compat/msvc_compat.h" \
        -I"$B/vendor/compat" -I"$V" -I"$V/BG3Extender" -I"$N" \
        -I"$E/glm" -I"$E/imgui" -I"$E/rapidjson/include" -I"$B/external/lua" \
        -I"$E/optick/src" -I"$E/tinycrypt/lib/include" -I/usr/include/SDL2 \
        "$V/$f" > "$OUT/$name.log" 2>&1
    n=$(grep -c "error:" "$OUT/$name.log")
    printf "%5s  %s\n" "$n" "$f"
}
export -f one

cd "$V" || exit 1
find . -name "*.cpp" | sed 's|^\./||' | sort | xargs -P 8 -I{} bash -c 'one "$@"' _ {}
