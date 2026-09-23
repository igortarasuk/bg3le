# Predicate analysis (2026-09-23)

Raw dumps: `reference/ghidra_predicate_full.txt`. Addresses raw VMA unless
marked `g` (Ghidra = raw + 0x100000). Tags: `[DISASM]` objdump, `[DECOMP]` Ghidra.

## Ghidra hygiene (persisted in project bg3proj)
- 1703 functions were flagged noreturn; cleared 274 (60 libc/thunk, 5 explicit, 209 `FUN_` with a RET in body), 1429 kept.
- 300697 `CALL_RETURN` flow overrides removed at 301127 call sites of the cleared functions.
- Bodies rebuilt for all targets: `FUN_038675f0` 102 -> 385 bytes, `FUN_063be100` 33 -> 1090, `FUN_051daf40` 97 -> 1315, `FUN_06ff8bd0` 459 -> 3360, etc. Scripts: `HygienePredicateFull.java`, `PredicateDecompile.java`, `PredicateClientSide.java`.

## Predicate `FUN_038675f0` (raw 0x37675f0) `[DECOMP][DISASM]`
- `bool IsOfficialModule(Module* m)`: snprintf GUID at `m+8`, FixedString ctor (`FUN_02538520`, raw 0x2438520), compare index against 19 `.bss` globals (SSE x16, then 3 scalar), release FixedString, return.
- Returns 1 for official, 0 for third-party. Prologue `41 57 41 56 53 48 83 ec 50`; patch `b8 01 00 00 00 c3` is safe (no callee-saved state before the compare result).
- Globals (raw): 7d5af30,34,38,3c,40,44,48,4c,50,54,58,5c,60,64,68,6c,70,7d5afe8,7d5afec. FixedString indices, assigned at startup by raw 0x4090271..0x4090639; GUID strings listed in the dump (Gustav 991c9c7a..., GustavDev 28ac9ce2..., Shared ed539163..., SharedDev 3d0c5ff8... `[HYP]` names).
- Loop `FUN_03867580` (raw 0x3767580): `HasCustomMods(list)`, returns 1 iff any of `list+0x14` entries (stride 0x60 at `list+8`) fails the predicate.
- Two more inlined copies exist, not covered by patching the predicate: `FUN_02d27ae0` (raw 0x2c27ae0, by-value variant; callers `FUN_02dfebe0`, `FUN_04385940`) and `FUN_02f06590` (raw 0x2e06590, module-settings validation; callers `FUN_02f05f60/6280/6520`, `FUN_031c6450`).

## Callers (list object, cache, role)
- `FUN_03867780` raw 0x3767780: loop on `gEocServer+0x268`; no cache; Osiris `UnlockAchievement` native -> `FUN_03867800` -> `FUN_03867890` sends NETMSG 0xb3.
- `FUN_03867800` raw 0x3767800: same list; no cache; helper of the above (also checks a peer object via `gEocServer+0xb0` vtable).
- `FUN_03867230` raw 0x3767230: same list; no cache; Osiris progress native, sends NETMSG 0xb9 (broadcast or to one peer).
- `FUN_04119c40` raw 0x4019c40: `obj+0xf8 ? 1 : loop(obj+0x20)` stored to `param_1+0x87`; savegame/session load, cached "IsModded" byte.
- `FUN_02d2cb20` raw 0x2c2cb20: reads `+0x87`, else loop on `obj+0x20`, else inlined loop on `gEocServer+0x108`; load-status/badge reporter, no store.
- `FUN_063be100` raw 0x62be100: loop on `[this+0x8b8]+0x198`, compares with byte `this+0x828`, stores on change and allocates an 0x18 notification; client mod-manager state change.
- `FUN_063bf260` raw 0x62bf260 (call at 0x62c04f3): predicate per entry of `obj+0x28/+0x34` (also reads `obj+0xf9`); client mod list UI filtering; no cache.
- `FUN_063815e0` raw 0x62815e0 (call at 0x6286239, via jump table): predicate per entry of `[ctx+0x920]+0x28/+0x34`, then compares with a second list via raw 0x3f4eb60; mod list diff for UI; no cache.
- `FUN_051daf40` raw 0x50daf40 (calls 0x50db42c/0x50db4f9): predicate per entry of `obj+0xc0/+0xcc`, collects non-official entries into a UI collection; client mod panel; no cache.
- `FUN_06ff8bd0` raw 0x6ef8bd0: inlined loop over a copy of the module list, stores result in `settings+0xf8`; this is the writer of the `+0xf8` flag consulted by the two functions above; savegame/module-settings load.
- `FUN_0460c8d0` raw 0x450c8d0, `FUN_0761d440` raw 0x751d440: predicate on a single module, then "Generated/Public/<mod>/VirtualTextures" path handling; virtual-texture loading, no cache.
- `FUN_027ab5d0` raw 0x26ab5d0: collects official entries (index >= 1) of `list+8`; mod ordering helper.
- `FUN_05209d00` raw 0x5109d00: counts non-official entries of `[obj+0x8b0]` list and compares with a config limit at `[7e9d198]+0x142c`; mod-count warning UI.

## Network message and client handler `[DISASM]`
- Registration raw 0x3ce3530: id 0xb3 `eocnet::AchievementMessage` (0x40 bytes, vtable raw 0x7929bb8), id 0xb9 `eocnet::AchievementProgressMessage` (vtable raw 0x7929bf8). Layout from Serialize (raw 0x4cd16a0): `+0x28` u32 ids, `+0x34` count, `+0x38` int16 user id.
- Client ProcessMsg raw 0x34b2440 (g 0x35b2440); branch at raw 0x34b25fa..0x34b268b: maps `+0x38` to a local user index, gets the achievements manager via `[7b75b50]->+0x98->vtable[0x90]`, then calls `mgr->vtable[0xb8](id, user)` per id.
- Manager vtable raw 0x7ae9710: slot 23 (`+0xb8`) raw 0x5078630 `UnlockAchievement`: returns 0 if `this+0x84 == 0` (set to 1 at raw 0x6aa0afa after Steam stats arrive), skips if already unlocked (slot 24), then Steam object `this+0x40` slot 12 (`+0x60`) = raw 0x6aa1170 -> `ISteamUserStats::SetAchievement(def->name)` (vtable `+0x38`) and `StoreStats` (`+0x50`). Achievement table raw 0x7b183b0, 54 entries x 0x4a0.
- Answer: the client handler runs NO mod check. Neither the handler, the manager nor the Steam wrapper reference `FUN_038675f0`, `FUN_03867580`, `+0x87`, `+0x828` or the official table. The only client gate is `mgr+0x84` (Steam stats loaded).

## Consequence for the patch
- Server-side gate is the predicate; patching `FUN_038675f0` to `mov eax,1; ret` opens the Osiris natives and every loop-based consumer. The inlined copies in `FUN_02d27ae0` and `FUN_02f06590` stay (module-settings validation / by-value check); patch them too only if "achievements disabled" badge or load validation still trips.
- Verify with a breakpoint at raw 0x34b2607 (client handler) and raw 0x6aa1199 (SetAchievement vtable call).
