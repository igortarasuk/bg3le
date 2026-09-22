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
# GameMode is off by default; GAMEMODE=1 turns it on. See the comment further
# down for why it is off.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
# The Steam install is the native build now, so the game and its Data live in
# one place and the separate tree with a Data symlink is gone.
GAME="$HOME/.local/share/Steam/steamapps/common/Baldurs Gate 3"
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

# Nothing sets SDL_VIDEODRIVER here on purpose: setting it on the command line
# already reaches the game, since pressure-vessel passes the environment
# through. The bundled libSDL2.so has both backends compiled in and the sniper
# runtime carries libwayland-client, so either is reachable --
# SDL_VIDEODRIVER=wayland for a native surface, x11 to force XWayland.
#
# Neither explains the frametime problem. A Proton DX11 run and a Proton Vulkan
# run share one windowing path and only one of them stutters, so the stutter
# does not track the window system.

cd "$GAME"

# Without steam_appid.txt beside the binary, libsteam_api does not know which
# app this is and asks Steam to launch it properly. Steam then starts the game
# itself -- without our LD_PRELOAD -- and the shell that ran this script is
# told "command line was forwarded" and exits. The game comes up either way,
# which is what makes this worth guarding: the only visible symptom is that
# bg3le is silently absent and bg3lua finds no server.
#
# Written here rather than left as a manual step because a Steam update can
# remove it again.
if [ ! -f steam_appid.txt ]; then
    printf '1086940' > steam_appid.txt
    echo "run-native: wrote steam_appid.txt (Steam install lacked it)" >&2
fi

launch=("$SNIPER/run" -- ./bin/bg3 "$@")

# GameMode is off unless asked for, because it does not work here and says so
# loudly. Inside the sniper container libgamemodeauto cannot dlopen
# libgamemode.so -- the container has its own /usr/lib, and the host's copy is
# not in it -- and cannot reach the session bus. The result was around 350
# lines of "dlopen failed" and "Could not connect to bus" per launch, and
# `gamemoded -s` still reporting "gamemode is inactive": it was never applying
# anything. An earlier note in this repo said otherwise on the strength of
# libgamemodeauto appearing in /proc/<pid>/maps; being mapped is not the same
# as working.
#
# Little is lost. What GameMode mainly does is set the CPU governor, and this
# machine already runs governor and energy_performance_preference at
# performance with the firmware profile at performance too. Its GPU
# optimisations need explicit opt-in and are not configured.
if [ "${GAMEMODE:-0}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
    launch=(gamemoderun "${launch[@]}")
fi

LD_PRELOAD="$HERE/build/libbg3le.so" \
BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le.log}" \
exec "${launch[@]}"
