# Achievements patch: live test runbook

Also kept in `~/Dev/bg3-modding/tools/gdb_scripts`; paths below assume that
layout.

Tooling for validating the bg3se-style predicate patch
(`FUN_038675f0` at raw VMA `0x37675f0` -> `mov eax,1; ret`) described in
`bg3le/reference/ACHIEVEMENTS-NEXT.md`. Paths below are relative to the
repository root `/home/itarasiuk/Dev/bg3-modding` unless absolute.

Game binary: `/mnt/data/SteamLibrary/steamapps/common/Baldurs Gate 3/bin/bg3`.
It is a PIE (`readelf -h` says `DYN`) whose first `PT_LOAD` has vaddr 0, so
the load bias is simply the mapped base of `bin/bg3` in `/proc/<pid>/maps`.
All scripts compute this themselves; nothing here assumes a fixed base.

## Ground rules

- bg3 runs as two processes, a parent and a `fork()`ed child. Game logic
  (`gEocServer`, Osiris natives) runs in the **child**. Always confirm which
  PID is which with `tools/gdb_scripts/bg3_pids.sh`.
- Both processes write to one log, `/tmp/bg3le.log.<parentpid>`. The file
  name says nothing about which process wrote a line.
- Passwordless sudo exists only for `/usr/bin/gdb`. Invoke it as
  `sudo -n /usr/bin/gdb ...` directly. Never wrap it in `env`, `timeout`,
  `nice` or a shell script, or sudo will prompt for a password. Timeouts
  live inside the generated gdb scripts (`gdb.post_event` timer).
- Anything that reads `/proc/<pid>/mem` needs root. `check_patch.py` is
  either run with `sudo python3 ...` (this will prompt; there is no NOPASSWD
  rule for python) or with `--gdb`, which routes the reads through the
  allowed gdb binary.
- Environment variables must reach the `bg3` process inside the Steam runtime
  container. Put them in the Steam launch options in front of `%command%`
  (`BG3LE_NO_ACH_PATCH=1 %command%`) or in whatever launcher you use to
  preload `libbg3le.so`.

## Files in this directory

| File | Purpose |
|---|---|
| `bg3_pids.sh` | Prints `PARENT=<pid> CHILD=<pid>`; `--parent` / `--child` print one PID. Exits 1 with a message if not exactly two bg3 processes. |
| `check_patch.py` | Reads the patch sites in both PIDs and reports ORIGINAL / PATCHED / UNKNOWN. |
| `steam_bt_gen.py` | Generates a gdb script that breakpoints an absolute address (the `SetAchievement` hook target), logs thread, achievement name and a full `bt` per hit, continues, auto-detaches. |
| `entry_watch_gen.py` | Older generator: non-interrupting entry breakpoint on a raw VMA, logs `rdi`/`rsi`. Use it in step 2 of ACHIEVEMENTS-NEXT.md to watch chain frames. |

## (a) The Steam reset helper

Source: `tools/reset_achievements.c`. A built binary already exists at
`tools/reset_achievements` (same mtime as the source, 2026-09-23 18:08). If
it is missing or older than the source, rebuild:

```sh
cd /home/itarasiuk/Dev/bg3-modding/tools
BG3BIN="/mnt/data/SteamLibrary/steamapps/common/Baldurs Gate 3/bin"
cc -o reset_achievements reset_achievements.c \
    -L"$BG3BIN" -lsteam_api -Wl,-rpath,"$BG3BIN"
```

It needs the real Steam client running and the app id, either as
`steam_appid.txt` containing `1086940` in the current directory or as
`SteamAppId=1086940` in the environment. Run it while the game is **not**
running, or Steam may report stale state.

```sh
cd /home/itarasiuk/Dev/bg3-modding/tools
SteamAppId=1086940 ./reset_achievements --list            # API name, earned, title
SteamAppId=1086940 ./reset_achievements --clear BG3_Quest33   # clear one
```

`--list` is how you map a localized title (for example "Важка доля") to
its `BG3_QuestNN` API name. Never use `--reset-all` during these tests. It
wipes every stat and achievement.

Pick one cleared achievement for the whole session and note its name; the
examples below use `BG3_Quest33`.

Clear only while the game is closed. The client achievements manager (raw
`0x5078630`) caches "already unlocked" at startup and silently skips such
ids, so a clear done while the game runs makes the engine path look blocked
when it is not. This may have contributed to the session 7 result.

## (b) Test A: the Load Game badge (cheapest, no gameplay)

Exercises the cached `+0x87` "IsModded" byte set at session load
(`FUN_04119c40`) and read by the load-status reporter (`FUN_02d2cb20`).

1. Build bg3le from the tree that carries the predicate patch
   (`ensure_achievement_gate_patch` in `bg3le/src/preload.cpp`).
2. Launch the game normally with bg3le preloaded and mods enabled.
3. In the shared log, confirm the patch was applied before `main`:
   `grep -n "EnableAchievements" /tmp/bg3le.log.*`
   Expected: `EnableAchievements: patched IsModded predicate at 0x37675f0`.
   A `WARNING: EnableAchievements predicate patch refused` line means the
   prologue bytes did not match and the rest of this runbook is moot.
4. Main menu -> Load Game. Select a save that was created with mods active.
5. Expected: the "achievements disabled" badge / warning is **absent**.
6. Control: repeat steps 2-5 once with `BG3LE_NO_ACH_PATCH=1`. The badge
   must be present again. Without this control the result means nothing.
7. Run `check_patch.py` (section (d)) while the game is at the menu, to
   prove both PIDs really carry `b8 01 00 00 00 c3`.

Record: patch on/off, badge present/absent, `check_patch` output.

## (c) Test B: engine-path unlock from the console

Goal: with bg3le's own forced-unlock bypass **off**, the engine's real
Osiris handler (`FUN_03867780`) must now pass `HasCustomMods` and reach
`ISteamUserStats::SetAchievement`.

Which env var turns the bypass off depends on the build you run:

- Tree at commit `c1187b9` / `ba7a81e` (the shipped DIV-dispatch bypass):
  set `BG3LE_NO_FORCE_ACH=1`. `maybe_force_unlock_achievement` becomes a
  no-op.
- Current working tree (patch-impl branch): `maybe_force_unlock_achievement`
  and `BG3LE_NO_FORCE_ACH` were removed together with the bypass, so there
  is nothing to turn off. Setting the variable is harmless but does nothing.
  Only `BG3LE_NO_ACH_PATCH` exists there.

Steps:

1. Game not running. Clear the target achievement:
   `SteamAppId=1086940 ./reset_achievements --clear BG3_Quest33`, then
   `--list` and confirm `earned=no`.
2. Launch with mods enabled, bypass off (`BG3LE_NO_FORCE_ACH=1` if the build
   has it). Load a modded save into gameplay.
3. Open the Lua console. bg3le runs a bg3lua-compatible debugger server on
   `127.0.0.1:9998` (override with `BG3LE_DEBUG_PORT`). Two ways in:
   - `CreateConsole`: with `"CreateConsole": true` in
     `bin/ScriptExtenderSettings.json` next to the game binary, bg3le spawns a
     terminal running `bg3le/client/bg3lua` at startup. The `client/`
     submodule was checked out on 2026-09-23 (`bg3le/client/bg3lua`,
     a dependency-free Python 3 script). `bin/ScriptExtenderSettings.json`
     does not exist in this install, so the manual way below is simpler.
   - Manual: from any terminal, `bg3le/client/bg3lua --host 127.0.0.1 --port 9998`
     (or `bg3lua -e '<one line>'` for a one-shot).
4. At the `server>` prompt, get the host character GUID and fire the unlock:

   ```lua
   Osi.GetHostCharacter()
   Osi.UnlockAchievement("BG3_Quest33", Osi.GetHostCharacter())
   ```

   The second argument is a GUIDSTRING such as
   `2c8e709e-01da-5c94-c989-475872f32125`, which is what
   `Osi.GetHostCharacter()` returns; you may paste it as a literal string.
   Known bug: calling `Osi.UnlockAchievement` on an **already earned**
   achievement crashes the game (COsiArgumentDesc marshalling bug in
   `src/osi.cpp`). Always clear first.
5. Expected within a few seconds:
   - a Steam overlay toast for the achievement;
   - in the shared log, `ISteamUserStats::SetAchievement(` ... followed by
     `-> true` and a `SetAchievement call` stack dump from `dump_own_stack`;
   - after quitting the game, `reset_achievements --list` shows the
     achievement `earned=yes`.
6. If the toast does not appear but the log shows `SetAchievement -> true`,
   Steam accepted it; check `--list`. If the log has no `SetAchievement`
   line at all, the gate is still closed somewhere past the predicate: go to
   section (e) and step 2 of ACHIEVEMENTS-NEXT.md.
7. Optional third confirmation: one gameplay-triggered unlock (the same
   achievement, cleared again), to rule out console-only paths.

## (d) Checking the patch bytes in both processes

```sh
cd /home/itarasiuk/Dev/bg3-modding/tools/gdb_scripts
./bg3_pids.sh                          # PARENT=... CHILD=...
sudo python3 check_patch.py            # direct /proc/<pid>/mem read, needs root
python3 check_patch.py --gdb           # same, via sudo -n /usr/bin/gdb (no password)
python3 check_patch.py --gdb 1234 1235 # explicit PIDs
```

Output per PID: its `PPid`, the computed bias and two lines:

```
  predicate 0x37675f0          PATCHED  b8 01 00 00 00 c3 83 ec 50
  consumer branch 0x37677a3    ORIGINAL 75 43
```

- `predicate` is the new patch: ORIGINAL `41 57 41 56 53 48 83 ec 50`,
  PATCHED `b8 01 00 00 00 c3` (the trailing three bytes are leftovers of the
  old prologue and are expected).
- `consumer branch` is the Session 7 NOP of the `jne` in `FUN_03867780`:
  ORIGINAL `75 43`, PATCHED `90 90`. With the current tree it should read
  ORIGINAL. If it reads PATCHED you are running the Session 7 build.
- UNKNOWN means neither pattern matched. Stop and compare against
  `objdump -d --start-address=0x37675f0 --stop-address=0x3767600 bin/bg3`.

Both PIDs must agree. A PATCHED parent with an ORIGINAL child would mean the
patch was applied after the fork, which contradicts the design (it is applied
from `bg3le_init`, before `main`).

The `--gdb` path attaches and detaches once per site, which pauses the game
for a fraction of a second each time. Prefer it at the main menu.

## (e) Mods-off baseline: capture the SetAchievement consumer chain

This has never been recorded and is the reference for step 2 of
ACHIEVEMENTS-NEXT.md. Do it with **mods disabled** (vanilla profile, no mod
list) but bg3le still preloaded, so the vtable hook and log exist.

1. Clear the target achievement (section (a)).
2. Launch the game, load an unmodded save.
3. Find the hook target address in the shared log:
   ```sh
   grep -h "SetAchievement slot" /tmp/bg3le.log.*
   # steam hook: ISteamUserStats vtable 0x..., SetAchievement slot -> 0xHOOK (was 0xREAL)
   ```
   `0xHOOK` is bg3le's `hooked_set_achievement`. Breaking there catches every
   caller and the frames above it are the engine's consumer chain.
   `0xREAL` is the original thunk inside `libsteam_api.so`. Either works;
   `0xHOOK` is one frame closer to the caller and is the default choice.
4. Generate and run the watcher against the **child** PID:
   ```sh
   cd /home/itarasiuk/Dev/bg3-modding/tools/gdb_scripts
   CHILD=$(./bg3_pids.sh --child)
   python3 steam_bt_gen.py "$CHILD" 0xHOOK /tmp/steam_bt_child.log steambt 600
   sudo -n /usr/bin/gdb -batch -x steambt.gdb
   ```
   Arguments: `<pid> <hex_addr> <out_log> <tag> [timeout_s] [--raw]`.
   The address is absolute as printed in the log. Pass `--raw` to give a raw
   VMA instead and have the bias added (useful later for chain frames).
   The script prints `[steambt] armed` and then blocks until the timeout,
   after which it detaches on its own. Ctrl-C in that terminal also detaches.
   If unsure which process fires, run a second instance against the parent
   in another terminal with a different tag and log path.
5. Trigger the unlock: either the console call from section (c), or a real
   gameplay unlock.
6. Read `/tmp/steam_bt_child.log`. Each hit gives
   `HIT #n thread lwp=<tid> rdi(this)=0x... name=BG3_Quest33` and a full
   `bt`. Frames inside `bg3` are symbolized from `.symtab`; frames in
   `libsteam_api.so` will be bare addresses. Save the file under
   `bg3le/reference/` as the mods-off baseline (for example
   `reference/steam_bt_modsoff.txt`), together with the log line from step 3
   and the load bias printed by the script, so raw VMAs can be recovered.
7. Later, with mods on and the patch active, repeat with the same tag and
   compare. Two known chain points to watch with `entry_watch_gen.py`
   (`--raw`): raw `0x34b2607` (client `ProcessMsg` achievement branch) and
   raw `0x6aa1199` (the `SetAchievement` vtable call in the Steam wrapper).
   Gate #2 on the server is `FUN_03867800` raw `0x3767800`. Then place `entry_watch_gen.py` breakpoints on each frame of the
   baseline chain (raw VMA = absolute - bias); the first frame that is never
   hit is the remaining gate.

## (f) A/B with BG3LE_NO_ACH_PATCH=1

Every conclusion needs the same trigger in both configurations, same PID
role (child), same save.

| Run | Env | Expect on badge (b) | Expect on console unlock (c) |
|---|---|---|---|
| A | patch on (default) | absent | toast, `SetAchievement -> true`, `--list` earned |
| B | `BG3LE_NO_ACH_PATCH=1` | present | no `SetAchievement` line, `--list` not earned |

Procedure per run:

1. Quit the game, clear the achievement, confirm `earned=no`.
2. Launch with the run's env. Confirm in the log: run A shows
   `EnableAchievements: patched IsModded predicate`; run B shows
   `EnableAchievements: BG3LE_NO_ACH_PATCH set, predicate left unpatched`.
3. `check_patch.py --gdb`: run A both PIDs PATCHED, run B both ORIGINAL.
4. Do (b) then (c). Optionally run the section (e) watcher during (c) to
   collect the mods-on backtrace for the same trigger.
5. Record results in `bg3le/reference/ACHIEVEMENTS-DIAGNOSIS.md` with the
   run letter, PIDs, bias, and the log excerpts.

If run A behaves like run B on the badge, the `+0x87` cache is set through a
path that does not call the predicate (see `*(obj+0xf8) != 0` in
`FUN_04119c40`). If the badge passes but the console unlock still produces
no `SetAchievement`, the missing gate is downstream, and section (e) is the
way to find it.
