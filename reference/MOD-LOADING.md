# What the engine loads, and what bg3le loads

Written 2026-09-23 after installing the 57-mod Windows backup into the
native Linux install, and revised the same day when the cause turned out
to be something else entirely.

## Where mods go

- Paks: `~/.local/share/Larian Studios/Baldur's Gate 3/Mods`
- Load order: `.../PlayerProfiles/Public/modsettings.lsx`

Established by A/B, not by assumption. With the paks in the profile's
`Mods` directory the engine reported **73 available mods** (16 base + 57);
with the same paks in `<install>/Data/Mods` it reported **16**. That
directory holds unpacked module trees — here only `GustavX/` — and the
engine does not read paks from it.

## Why the engine loaded only one mod, and what fixed it

For a whole session of testing, the engine's load order held 14 modules:
the 13 base ones and Mod Configuration Menu. The other 56 mods were
listed in `modsettings.lsx`, present in `Mods`, and reported as available
— and never loaded.

The cause was **`ModCrashSanityCheck`**. The game writes that directory
into the profile while it runs and removes it on a clean exit; finding one
at startup is how it decides the last run crashed, and it disables mods
when it does. Every test run here ends with the game being killed, so the
marker was there every time. bg3se removes it at startup for exactly this
reason (`CleanupSanityCheck` in `ScriptExtenderClient.cpp`); bg3le did
not. It does now, and the next run's load order held **69 modules**.

Attributed rather than assumed: `BG3LE_KEEP_SANITY_CHECK=1` leaves the
marker in place, and two runs back to back with the same load order and
the same save gave 14 modules with it and 69 without. Osiris' node count
moves with it too -- 151,453 against 153,863 -- and 153,863 is what bg3se
reported for the same save on Windows, which is a useful check that both
are reading the same story.

The single exception is worth keeping, because it is what made the cause
hard to see: MCM loaded even with mods disabled. That is content-driven
mounting rather than the load order. Stripping MCM down establishes it:

| Archive | Loaded |
| --- | --- |
| MCM as shipped, and repacked uncompressed under another folder name | yes |
| MCM's `Mods/` tree alone | **no** |
| MCM's `Mods/` tree plus its `Localization/` | yes |
| MCM's `Mods/` tree plus its `Public/` | yes |
| MCM's `Mods/` + `Public/` plus another mod's `Public/<Folder>/` | yes |

MCM overrides `Public/Shared/GUI` and `Public/Shared/Content/UI`, which
the main menu reads. Every other mod in the set keeps its content under
`Mods/<Folder>/` or `Public/<Folder>/`, which nothing at the menu touches.

Things ruled out along the way, each by a launch with a purpose-built
archive (`tools/modsettings --repack/--only/--without/--merge/--rename/--make`):
the metadata (MCM's meta.lsx shape, version, attribute types, even its
UUID), the folder name, the pak's priority byte, its MD5, and its
compression. None of them changes the answer.

Two more things worth knowing:

- **A savegame's module list replaces the load order.** Loading a
  mod-free save rewrote `modsettings.lsx` down to `GustavX` — which is
  how the Windows profile in the backup came to list no mods at all.
- **One malformed archive stops the engine loading any mod.** An early
  version of `pak_write` put the compressed size in the header's
  file-list size field, which counts the whole block including the two
  leading counts. With that archive present, MCM stopped loading too.

## What bg3le loads

Mod scripts, from inside the paks, for every mod in the engine's load
order and then for anything else enabled in `modsettings.lsx`. The second
half is a deliberate divergence from upstream; it is what kept the five
script mods running while the engine was loading none of them, and it is
harmless now that the engine loads them.

Each mod gets `Mods[ModTable]` as its environment with the real globals
behind it, `ModuleUUID` set for the duration of its bootstrap and left in
its table afterwards, `require` resolving against its own `Lua/`
directory, and `Ext.IO.LoadFile(path, "data")` falling through to the
archives when the file is not on disk.

`BootstrapClient.lua` is not run at all: bg3le has one Lua context, the
server's. For a UI mod that is most of the mod, so the mods that ship one
are named at load time.
