#!/bin/bash
# Run the native build directly on the host, outside the Steam runtime
# container, to test whether pressure-vessel is implicated in the slow load.
#
# The container is otherwise only needed for OpenSSL 1.1, which CachyOS no
# longer ships; borrow those two libraries from the sniper tree. Everything
# else (glibc, graphics drivers) then comes from the host, which is the
# point of the comparison.
set -e
SNIPER="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"
COMPAT="$SNIPER/sniper_platform_3.0.20260805.254768/files/lib/x86_64-linux-gnu"

if [ ! -f "$COMPAT/libssl.so.1.1" ]; then
    echo "OpenSSL 1.1 not found under $COMPAT" >&2
    exit 1
fi

# Only the two missing libraries, not the whole runtime: adding the rest
# would defeat the experiment by reintroducing the container's stack.
SHIM=/tmp/bg3-ssl-shim
mkdir -p "$SHIM"
ln -sf "$COMPAT/libssl.so.1.1" "$SHIM/libssl.so.1.1"
ln -sf "$COMPAT/libcrypto.so.1.1" "$SHIM/libcrypto.so.1.1"

cd /home/lenon/bg3mods/bg3-linux-native
export LD_LIBRARY_PATH="$SHIM:$PWD/bin${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MANGOHUD="${MANGOHUD:-0}"

# BG3LE_NO_STEAM_LAYERS=1 disables Steam's implicit Vulkan layers (shader
# pre-caching and the overlay). They are injected even outside the runtime
# container and are a known source of load-time stalls.
if [ "${BG3LE_NO_STEAM_LAYERS:-0}" = "1" ]; then
    export DISABLE_VK_LAYER_VALVE_steam_overlay_1=1
    export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
    export DISABLE_LAYER_AMD_switchable_graphics_1=1
    echo "Steam Vulkan layers disabled for this run" >&2
fi

# Attach the extender purely for its timing report, so this run is directly
# comparable with the sniper one. The control run already established it is
# not responsible for the slow load.
HERE="$(cd "$(dirname "$0")/.." && pwd)"
export LD_PRELOAD="$HERE/build/libbg3le.so"
export BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le-bare.log}"

exec ./bin/bg3 "$@"
