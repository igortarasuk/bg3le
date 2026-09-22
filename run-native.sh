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

# GameMode, on by default when installed. GAMEMODE=0 turns it off.
#
# It wraps the sniper runtime rather than going inside it. gamemoderun works by
# preloading libgamemodeauto.so.0, and that library is on the host and not in
# the container -- the same gap that made the libtbb dependency untenable. The
# request therefore comes from the process outside, which is enough: the
# governor and scheduler changes are made by gamemoded, not by the game.
#
# The container still inherits a preload it cannot resolve, so expect one
#
#   ERROR: ld.so: object 'libgamemodeauto.so.0' from LD_PRELOAD cannot be
#   preloaded (cannot open shared object file): ignored.
#
# per process in there. The loader ignores it and carries on: unlike a missing
# linked dependency, an unresolvable preload is not fatal.
#
# What this means in practice is worth knowing. gamemode's global effects --
# CPU governor, GPU power profile, screensaver inhibit, core parking -- are
# applied by gamemoded and work whichever process registered, so they apply.
# Its per-process effects -- renice, ioprio, soft-realtime scheduling -- are
# applied to the registering pid, which here is the wrapper outside the
# container rather than the game. The same is true of "gamemoderun %command%"
# in Steam's launch options, for the same reason.
#
# Pointing the preload at the host copy through /run/host would put the game
# itself in gamemode, and does not work: the host library is built against a
# much newer glibc than the container's, and it dlopens libgamemode.so.0,
# which needs dbus. Check what is actually in effect with
#
#   gamemoded -s            # globally
#   gamemoded -s <game pid> # for the game itself
#
# Order matters. gamemoderun prepends itself to whatever LD_PRELOAD it
# inherits, so the shim has to be in its environment already or it would be
# replaced rather than joined -- which is why the assignments sit in front of
# gamemoderun rather than in front of the runtime.
launch=("$SNIPER/run" -- ./bin/bg3 "$@")
if [ "${GAMEMODE:-1}" != "0" ] && command -v gamemoderun >/dev/null 2>&1; then
    launch=(gamemoderun "${launch[@]}")
fi

LD_PRELOAD="$HERE/build/libbg3le.so" \
BG3LE_LOG="${BG3LE_LOG:-/tmp/bg3le.log}" \
exec "${launch[@]}"
