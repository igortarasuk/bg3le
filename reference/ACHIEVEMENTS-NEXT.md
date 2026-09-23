# Achievements with mods: state of knowledge and next vector

Written 2026-09-23 after reviewing ACHIEVEMENTS-DIAGNOSIS.md (sessions 1-7)
and the binary. Read this file first; the diagnosis file is
a diary, not a spec. Every claim below is tagged:
`[LIVE]` confirmed in a running process, `[DISASM]` read from objdump output,
`[DECOMP]` from a decompiler, `[HYP]` hypothesis.

## TL;DR

The mod check is not one branch. It is one small predicate, "is this module
official", called from a loop "does any module fail the predicate", and that
loop is consulted by at least four independent consumers, two of which cache
the answer in a byte. Session 7 NOP'd one consumer's branch and therefore
changed nothing observable. bg3se on Windows patches the predicate level, not
a consumer, and that is what bg3le should do too.

## Verified facts

| Raw VMA | Label | What it is | Tag |
|---|---|---|---|
| `0x37675f0` | `FUN_038675f0` | Per-module predicate. Formats the module GUID at `entry+8` as `%08x-%04hx-...`, builds a FixedString from it, then compares that FixedString against a packed table of official-module constants with SSE (`cmp ecx,[rip+..]; mov al,1; ... sete dl`). Returns 1 for an official module. The real body continues past the FixedString call to about `0x37677xx`. | `[DISASM]` |
| `0x3767580` | `FUN_03867580` | `HasCustomMods(list)`. `list+0x14` = count, `list+8` = entries, stride `0x60`. Returns 0 if the list is empty or every entry passes the predicate, 1 if any entry fails it. Prologue bytes `55 41 57 41 56 41 55 41 54 53 50`. | `[DISASM]` |
| `0x3767780` | `FUN_03867780` | Registered Osiris native for `UnlockAchievement` (id `0x80001669`). `if (!HasCustomMods(gEocServer+0x268)) FUN_0398e8e0(arg->+0x18)`. Hit 8 times in the child process during one achievement event, always with the check returning 1 while mods were active. | `[LIVE]` `[DECOMP]` |
| `0x3767800`, `0x3767230` | `FUN_03867800`, `FUN_03867230` | Sibling Osiris natives (achievement progress queries by shape), same `HasCustomMods(gEocServer+0x268)` guard. | `[DECOMP]` |
| `0x4019c40` | `FUN_04119c40` | Save/session load. `modded = (*(obj+0xf8) != 0) ? 1 : HasCustomMods(obj+0x20)`, stored into `param_1+0x87`. A cached per-session "IsModded" byte. | `[DECOMP]` |
| `0x2c2cb20` | `FUN_02d2cb20` | Load-status reporter. Reads the `+0x87` cache, and also has an inlined copy of the HasCustomMods loop over `gEocServer+0x108` calling the predicate directly. This is the Linux shape of bg3se's `ThrowError` AOB. | `[DECOMP]` |
| `0x62be2ca` | `FUN_063be100` | Third consumer: `HasCustomMods([r13+0x8b8]+0x198)`, compared with the cached byte `[r13+0x828]`; on change it stores the new value and allocates an 0x18-byte notification object. A second cached "IsModded" byte on a different object. | `[DISASM]` |
| `0x388e8e0` | `FUN_0398e8e0` | Called by the Osiris handler once the gate is open. Has 337 callers. It is a generic helper, not "the unlock". Session 6 labelled it "the real unlock path" without decompiling it. | `[DECOMP]` |
| `.rodata` | strings | `eocnet::AchievementMessage`, `eocnet::NETMSG_ACHIEVEMENT_UNLOCKED_MESSAGE`, `eocnet::AchievementProgressMessage` exist. The unlock travels server -> network message -> client; the Steam call is on the client side and asynchronous to the Osiris handler. | `[DISASM]` `[HYP]` on the flow |
| process | topology | Two `bg3` PIDs, a real `fork()`. Game logic that touches `gEocServer` runs in the child. The child writes to the parent-named `bg3le.log.<parentpid>` through the inherited `FILE*`, so "the parent's log" is really the shared log. Attach gdb to the child. | `[LIVE]` |
| DIV bypass | commit `c1187b9` | Hooking Osiris dispatch for id `0x80001669` and calling `ISteamUserStats::SetAchievement` directly works with mods active. Kept as the fallback. | `[LIVE]` |

Address convention: `FUN_0xxxxxxx` labels are raw VMA + `0x100000` (the
convention the analysis was recorded in). Patching code uses raw VMA plus
the load bias, as `hook.cpp` already does.

## Why the search kept failing

1. Labels were assigned before decompiling. "Real unlock path" for
   `FUN_0398e8e0` and "count != 0" for `FUN_03867580` were both wrong and each
   cost a session.
2. The decompiler database had bogus `noreturn` flags on `strlen`, the
   FixedString ctor and several helpers, so every decompile stopped at the
   first such call and `FUN_038675f0` looked like a void function.
3. Forward search from the Osiris handler on a path that is asynchronous and
   crosses server -> client. There is no single call chain to follow.
4. The working (mods-off) chain from `SetAchievement` backwards was never
   recorded, so there was nothing to diff against.
5. Live experiments on the parent PID produced "zero hits" that were read as
   "not consulted" (the SSE GUID check in session 2 was exactly this
   predicate).

## Plan, in order

### Step 0: fallback

The DIV bypass in `c1187b9` remains plan B only. It is not needed after the
validation below.

### Step 1: patch the predicate, bg3se style (expected: 1-2 hours)

Patch `FUN_038675f0` at raw VMA `0x37675f0` to `mov eax,1; ret`
(`b8 01 00 00 00 c3`). Expected prologue bytes for the safety memcmp:
`41 57 41 56 53 48 83 ec 50`. Use the existing `patch_bytes()` in
`src/hook.cpp`. Apply it in the preload constructor, before `main`, so the
patch is inherited by the fork and precedes every session-load cache.

**Implemented 2026-09-23:** `src/preload.cpp`, `ensure_achievement_gate_patch()`
(constants `kAchievementPredicate*`), called first from the `bg3le_init`
constructor, then re-checked after `RegisterDIVFunctions` and every server
tick. Uses the new five-argument `patch_bytes()` overload in `src/hook.cpp`
(verify 9 bytes, write 6). Opt-out for A/B runs: `BG3LE_NO_ACH_PATCH=1`.
**Validated live 2026-09-23** (`reference/live_validation_2026-09-23.txt`):
with mods active, killing the Adamantine Golem unlocked `BG3_Quest33` with a
real Steam toast; the "no achievements" and "modded" badges on the Load Game
list disappeared. The `SetAchievement` backtrace matches the mapped chain
exactly: Steam wrapper `0x6aa119c`, manager `0x50786ef`, client `ProcessMsg`
`0x34b267f`. The crash on first launch was fixed by patching only when
`/proc/self/exe` is `bg3` and by bounds-checking offsets against `.text`
(`LD_PRELOAD` also lands in `steam-launch-wrapper` and `reaper`).

Alternative if the predicate is needed elsewhere: patch `FUN_03867580` to
`xor eax,eax; ret` (`31 c0 c3`), which covers its 7 direct callers but not the
inlined loop in `FUN_02d2cb20`.

Test protocol, cheapest first:
1. Open Load Game with a modded save. The "achievements disabled" badge must
   disappear. This exercises the `+0x87` cache with no gameplay.
2. `tools/reset_achievements --clear BG3_QuestNN`, then call
   `Osi.UnlockAchievement` from the console. This tree has no DIV bypass, so
   the engine path is the only one. Expect a Steam toast and `--list` showing
   earned, and a `SetAchievement` line with a backtrace in the shared log.
3. One gameplay-triggered unlock.

### Step 2 (not needed after validation): diff the working chain

1. Mods off, child PID, breakpoint on `ISteamUserStats::SetAchievement` (the
   vtable hook already calls `dump_own_stack`). Record the full backtrace with
   thread id. This is the consumer chain and has never been captured.
2. Mods on with the step 1 patch, breakpoints on every frame of that chain
   (`tools/gdb_scripts/entry_watch_gen.py`). The first frame that is not hit
   is the gate. Decompile only that function, after clearing noreturn flags.

### Step 3: if the chain crosses threads or is hard to place, Intel PT

The CPU exposes `intel_pt` (`/sys/bus/event_source/devices/intel_pt`).
`perf` is packaged as `extra/perf` but not installed;
`kernel.perf_event_paranoid` is 2, so run it under sudo. Record the child PID
for a few seconds around the unlock in both configurations with an address
filter on the `bg3` image, decode with `perf script --itrace=b`, and diff the
sets of executed branch addresses. The first divergent branch is the gate.
This replaces static guessing entirely.

## Update 2026-09-23 evening: full chain mapped, session 7 explained

Details in `PREDICATE-ANALYSIS.md`. The complete path, all
`[DECOMP]`/`[DISASM]`:

1. Server, Osiris native `UnlockAchievement` `FUN_03867780` (raw `0x3767780`):
   `HasCustomMods(gEocServer+0x268)` gate #1, then `FUN_0398e8e0` resolves
   the character GUID string to an entity handle (generic helper, 337
   callers), then `FUN_03867800`.
2. `FUN_03867800` (raw `0x3767800`): **`HasCustomMods(gEocServer+0x268)`
   again, gate #2**, then looks the achievement name up through the
   manager at `gEocServer+0xb0` (vtable `+0xa8`), then `FUN_03867890`.
3. `FUN_03867890` (raw `0x3767890`): sends `eocnet::AchievementMessage`,
   net id `0xb3`, to the character's user peer.
4. Client `ProcessMsg` raw `0x34b2440`, branch `0x34b25fa..0x34b268b`: maps
   the user id, gets the achievements manager, calls `mgr->vtable[0xb8]`.
5. Manager `UnlockAchievement` raw `0x5078630`: returns if `mgr+0x84 == 0`
   (Steam stats not received yet), returns if already unlocked in its own
   cache, else Steam wrapper raw `0x6aa1170` calls
   `ISteamUserStats::SetAchievement(def->name)` and `StoreStats`.

The client side never calls the predicate, the loop, or reads `+0x87` /
`+0x828`. There is no client-side mod check.

Why session 7 failed: it NOP'd gate #1 only. Gate #2 sits in
`FUN_03867800` two calls later and was never opened. A second possible
contributor: the client manager caches "already unlocked"; clearing an
achievement with `reset_achievements` while the game runs leaves that cache
stale, so the engine path skips silently while the direct-Steam bypass still
shows a toast. Always clear with the game closed.

Coverage of the predicate patch: opens gates #1 and #2 and every loop-based
consumer. Two inlined copies of the compare remain unpatched:
`FUN_02d27ae0` (raw `0x2c27ae0`, by-value variant) and `FUN_02f06590`
(raw `0x2e06590`, module-settings validation). They affect load validation
and possibly the badge, not the unlock. Patch them only if test A fails
while test B passes.

Live verification points (child PID): raw `0x34b2607` (client handler
branch) and raw `0x6aa1199` (the `SetAchievement` vtable call). Tooling and
protocol: `tools/gdb_scripts/RUNBOOK.md`.

## Rules for the next session

- Never label a function until it is decompiled with clean noreturn flags.
- Every conclusion needs an A/B: mods off and mods on, same PID, same trigger.
- Attach to the child PID. Verify with `PPid` in `/proc/<pid>/status`.
- Log lines are shared between both processes; do not infer process identity
  from the log file name.
- Keep the fallback bypass buildable at all times.

## Files added on 2026-09-23

- `reference/PREDICATE-ANALYSIS.md`: predicate, loop, all callers, network
  message layout, client handler and Steam wrapper, with addresses.
- `reference/live_validation_2026-09-23.txt`: log excerpt of the live test.
- `tools/gdb_scripts/`: `RUNBOOK.md`, `bg3_pids.sh`, `check_patch.py`,
  `steam_bt_gen.py`, `entry_watch_gen.py`.
- Settings parity: `"EnableAchievements": false` in
  `ScriptExtenderSettings.json` disables the patch (`settings_flag()` in
  `src/console.cpp`); `BG3LE_NO_ACH_PATCH=1` forces it off.

Decompiler databases, scripts and raw dumps are kept outside this repo.
