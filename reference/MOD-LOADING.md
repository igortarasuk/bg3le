# What the engine loads, and what bg3le loads

Written 2026-09-23, after installing the 57-mod Windows backup into the
native Linux install and finding that the game loaded exactly one of them.

## Where mods go

- Paks: `~/.local/share/Larian Studios/Baldur's Gate 3/Mods`
- Load order: `.../PlayerProfiles/Public/modsettings.lsx`

Established by A/B, not by assumption. With the paks in the profile's
`Mods` directory the engine reported **73 available mods** (16 base + 57);
with the same paks in `<install>/Data/Mods` it reported **16**. That
directory holds unpacked module trees — here only `GustavX/` — and the
engine does not read paks from it.

`modsettings.lsx` is read: Mod Configuration Menu appears in the load
order when it is listed there and disappears when the entry is removed.

## What the engine loads

Of the 57, the engine loads **Mod Configuration Menu** and nothing else,
at the main menu and with a savegame up, on this machine.

That is not bg3le's doing. `NOPRELOAD=1 ./run-native.sh` runs the game
with no shim at all, and `tools/memgrep <pid> <string>` reads its memory
from outside: the vanilla game has none of 5eSpells' stat names in
memory either, and the same fourteen modules in its list.

Ruled out, each by a launch with a purpose-built archive
(`tools/modsettings --repack/--only/--without/--merge/--rename/--make`):

| Changed | Result |
| --- | --- |
| The refused mod's meta given MCM's shape, version and attribute types | still refused |
| The refused mod's UUID put on MCM | MCM still loads |
| MCM's UUID and folder put on the refused mod | still refused |
| The pak's priority byte set to MCM's 100 | still refused |
| `LSWString` attributes rewritten as `LSString` | still refused |
| MCM repacked uncompressed, folder renamed throughout | still loads |
| MCM's `Mods/` tree alone | **refused** |
| MCM's `Mods/` tree plus its `Localization/` | loads |
| MCM's `Mods/` tree plus its `Public/` | loads |
| MCM's `Mods/` + `Public/` plus the refused mod's `Public/<Folder>/` | loads |

So it is the content, and specifically whether anything reads it where
you are looking. MCM overrides `Public/Shared/GUI` and
`Public/Shared/Content/UI`, which the main menu reads. Every other mod in
the set keeps its content under `Mods/<Folder>/` or `Public/<Folder>/`,
which nothing at the menu touches.

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
half is a deliberate divergence from upstream, and the reason the five
script mods in this set — BG3MCM, 5eSpells, LenonTweaks,
TashasFightingStyles, TashasRanger — run at all here.

Each mod gets `Mods[ModTable]` as its environment with the real globals
behind it, `ModuleUUID` set for the duration of its bootstrap and left in
its table afterwards, `require` resolving against its own `Lua/`
directory, and `Ext.IO.LoadFile(path, "data")` falling through to the
archives when the file is not on disk.
