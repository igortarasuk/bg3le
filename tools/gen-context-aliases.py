#!/usr/bin/env python3
"""Emits the Lua table describing Ext.Server*/Ext.Client* from the captured
surface, so the alias layer cannot drift from what the real extender does.

Writes to stdout; paste the result into the prelude in src/lua_host.cpp.
"""
import re
import sys

REF = "reference/ext-api-surface.txt"

mods = {}
for line in open(REF):
    m = re.match(r"^Ext\.(\w+) = \{(.*)\}$", line.strip())
    if m:
        mods[m.group(1)] = set(re.findall(r"(\w+)\(function\)", m.group(2)))

base = {k: v for k, v in mods.items()
        if not k.startswith(("Server", "Client"))}

rows = []
for name in sorted(mods):
    for prefix in ("Server", "Client"):
        if not name.startswith(prefix):
            continue
        short = name[len(prefix):]
        if short not in base:
            print(f"# {name} has no base module; left alone", file=sys.stderr)
            continue
        omitted = sorted(base[short] - mods[name])
        extra = sorted(mods[name] - base[short])
        if extra:
            print(f"# {name} has {extra} that Ext.{short} lacks",
                  file=sys.stderr)
        rows.append((name, short, omitted))

print("local CONTEXT_MODULES = {")
for name, short, omitted in rows:
    if omitted:
        omit = ", ".join(f'{o} = true' for o in omitted)
        print(f'  {{"{name}", "{short}", {{{omit}}}}},')
    else:
        print(f'  {{"{name}", "{short}"}},')
print("}")
