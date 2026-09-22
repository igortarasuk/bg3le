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

# Driver tuning, exported so it reaches the game inside the container.
#
# vk_x11_strict_image_count is a Mesa driconf option, and Mesa reads driconf
# options from an environment variable of the same name -- confirmed present in
# this machine's libvulkan_radeon.so.
#
# RADV_PERFOPTS is not a variable that driver reads. It reads RADV_DEBUG and
# RADV_PERFTEST, and "async_compile" appears nowhere in it; the RADV_PERFTEST
# options this build (Mesa 26.2.3) does carry include cswave32, gewave32,
# pswave32, nosam, nircache, transfer_queue, dccmsaa, localbos and
# video_decode. An unrecognised variable is simply ignored, so the line is a
# no-op rather than harmful -- kept as asked, and recorded here so it is not
# later mistaken for something that is doing work.
export RADV_PERFOPTS="${RADV_PERFOPTS:-async_compile}"
export vk_x11_strict_image_count="${vk_x11_strict_image_count:-false}"

cd "$GAME"

launch=("$SNIPER/run" -- ./bin/bg3 "$@")
if [ "${GAMEMODE:-1}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
    launch=(gamemoderun "${launch[@]}")
fi

LD_PRELOAD="$HERE/build/libbg3le.so" \
BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le.log}" \
exec "${launch[@]}"
