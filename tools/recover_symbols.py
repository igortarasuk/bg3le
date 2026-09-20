#!/usr/bin/env python3
"""Recover Larian function names from embedded assert strings.

Larian's own functions have internal linkage and no symbols, which is why the
Windows extender locates them with byte patterns. But the binary keeps the
string constants that __PRETTY_FUNCTION__ / __FUNCTION__ expand to, and their
local labels carry the mangled name. Code that references such a string is,
by construction, the function it names.

So: take each label's address, find the instruction that loads it, and
attribute that to the enclosing function via .eh_frame_hdr. The result is a
name -> address map that survives recompilation, unlike a byte signature.
"""

import re
import struct
import subprocess
import sys

LABEL_RE = re.compile(r"^\.L(?:__PRETTY_FUNCTION__|__FUNCTION__|__func__)\.(_Z\S+)$")

# REX.W lea reg, [rip+disp32] -- 48/4c 8d /r with mod=00 rm=101.
LEA_RE = re.compile(rb"[\x48\x4c]\x8d[\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", re.S)


def sections(path):
    out = {}
    text = subprocess.run(["readelf", "-S", "-W", path], capture_output=True,
                          text=True).stdout
    for line in text.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)",
                     line)
        if m:
            out[m.group(1)] = {"addr": int(m.group(2), 16),
                               "offset": int(m.group(3), 16),
                               "size": int(m.group(4), 16)}
    return out


def label_addresses(path):
    """mangled name -> address of its __PRETTY_FUNCTION__ string constant."""
    out = {}
    proc = subprocess.run(["nm", "--defined-only", path], capture_output=True, text=True)
    for line in proc.stdout.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        m = LABEL_RE.match(parts[2].strip())
        if m:
            out.setdefault(int(parts[0], 16), []).append(m.group(1))
    return out


def function_starts(path, secs):
    """Every function entry point, from the .eh_frame_hdr binary search table."""
    hdr = secs.get(".eh_frame_hdr")
    if hdr is None:
        return []
    with open(path, "rb") as f:
        f.seek(hdr["offset"])
        blob = f.read(hdr["size"])

    version, eh_enc, count_enc, table_enc = blob[0], blob[1], blob[2], blob[3]
    if version != 1 or count_enc != 0x03 or table_enc != 0x3b:
        # 0x03 = udata4 absptr, 0x3b = sdata4 | datarel: the usual encoding.
        print(f"unexpected .eh_frame_hdr encodings "
              f"{eh_enc:#x}/{count_enc:#x}/{table_enc:#x}", file=sys.stderr)
        return []

    count = struct.unpack_from("<I", blob, 8)[0]
    starts = []
    for i in range(count):
        loc = struct.unpack_from("<i", blob, 12 + i * 8)[0]
        starts.append(hdr["addr"] + loc)
    starts.sort()
    return starts


def main():
    if len(sys.argv) < 2:
        print("usage: recover_symbols.py <elf> [name-filter]", file=sys.stderr)
        return 2
    path = sys.argv[1]
    want = sys.argv[2] if len(sys.argv) > 2 else None

    secs = sections(path)
    text = secs[".text"]
    labels = label_addresses(path)
    starts = function_starts(path, secs)
    print(f"# {len(labels)} label addresses, {len(starts)} function entries",
          file=sys.stderr)

    with open(path, "rb") as f:
        f.seek(text["offset"])
        code = f.read(text["size"])

    import bisect
    from collections import defaultdict

    name_to_addrs = defaultdict(set)
    addr_to_names = defaultdict(set)
    for m in LEA_RE.finditer(code):
        pos = m.start()
        if pos + 7 > len(code):
            continue
        disp = struct.unpack_from("<i", code, pos + 3)[0]
        insn_addr = text["addr"] + pos
        target = insn_addr + 7 + disp
        names = labels.get(target)
        if not names:
            continue
        idx = bisect.bisect_right(starts, insn_addr) - 1
        if idx < 0:
            continue
        for name in names:
            name_to_addrs[name].add(starts[idx])
            addr_to_names[starts[idx]].add(name)

    # A function referencing an inlined callee's string would be misattributed,
    # so only accept unambiguous 1:1 pairs. Measured against functions that do
    # have real symbols, this raises accuracy from 89% to near-exact at the
    # cost of dropping the ambiguous minority.
    recovered = {}
    ambiguous = 0
    for name, addrs in name_to_addrs.items():
        if len(addrs) != 1:
            ambiguous += 1
            continue
        addr = next(iter(addrs))
        if len(addr_to_names[addr]) != 1:
            ambiguous += 1
            continue
        recovered[name] = addr
    print(f"# dropped {ambiguous} ambiguous attributions", file=sys.stderr)

    for name, addr in sorted(recovered.items(), key=lambda kv: kv[1]):
        if want and want not in name:
            continue
        print(f"{addr:#012x} {name}")
    print(f"# recovered {len(recovered)} functions", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
