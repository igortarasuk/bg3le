#!/usr/bin/env python3
"""Locate the function-pointer table slot holding each given function.

Virtual and callback-dispatched functions have their address stored in a
table (vtable, jump table, handler struct) via an R_X86_64_RELATIVE
relocation. Patching that slot is a single pointer write -- no code
patching, no instruction length decoding, and trivially reversible.

Usage: find_slots.py <elf> [name-substring]
Reads the recovered symbol map on stdin (output of recover_symbols.py).
"""

import re
import subprocess
import sys


def relative_relocs(path):
    """addend (function address) -> [slot addresses]"""
    out = {}
    proc = subprocess.run(["readelf", "-r", "-W", path], capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 4 and parts[2] == "R_X86_64_RELATIVE":
            try:
                slot = int(parts[0], 16)
                addend = int(parts[3], 16)
            except ValueError:
                continue
            out.setdefault(addend, []).append(slot)
    return out


def main():
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None

    relocs = relative_relocs(path)
    print(f"# {len(relocs)} distinct pointer-table targets", file=sys.stderr)

    for line in sys.stdin:
        if line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        addr_s, name = parts[0], parts[1].strip()
        if want and want not in name:
            continue
        addr = int(addr_s, 16)
        slots = relocs.get(addr)
        if not slots:
            continue
        for slot in slots:
            print(f"{slot:#012x} {addr:#012x} {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
