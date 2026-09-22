#!/bin/bash
# Launch the native Linux BG3 build with the extender shim attached.
#
# Shim logs go to $BG3LE_LOG.<pid> (default /tmp/bg3le.log.<pid>); the game's
# own process is the one whose log mentions COsiris.
#
# MangoHud is on by default. It loads as a Vulkan implicit layer rather than
# via its LD_PRELOAD shim, so it cannot collide with ours. MANGOHUD=0 or
# DISABLE_MANGOHUD=1 turns it off.
#
# GameMode is on by default when installed; GAMEMODE=0 turns it off. See the
# comment further down for what it does and does not reach.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
GAME=/home/lenon/bg3mods/bg3-linux-native
SNIPER="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"

export MANGOHUD="${MANGOHUD:-1}"
export BG3LE_DUMP_DB="${BG3LE_DUMP_DB:-1}"  # temporary: structural dump

cd "$GAME"

launch=("$SNIPER/run" -- ./bin/bg3 "$@")
if [ "${GAMEMODE:-1}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
    launch=(gamemoderun "${launch[@]}")
fi

LD_PRELOAD="$HERE/build/libbg3le.so" \
BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le.log}" \
exec "${launch[@]}"
