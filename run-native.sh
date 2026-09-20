#!/bin/bash
# Launch the native Linux BG3 build with the extender shim attached.
# Logs land in /tmp/bg3le.log.<pid>; the game's process is the one that
# mentions COsiris.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME=/home/lenon/bg3mods/bg3-linux-native
SNIPER="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"

cd "$GAME"
LD_PRELOAD="$HERE/build/libbg3le.so" \
BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le.log}" \
exec "$SNIPER/run" -- ./bin/bg3 "$@"
