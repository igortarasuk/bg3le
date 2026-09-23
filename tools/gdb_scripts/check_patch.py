#!/usr/bin/env python3
# Reports whether the EnableAchievements patch is live in both bg3 PIDs.
# Usage: sudo python3 check_patch.py [--gdb] [pid ...]   (see RUNBOOK.md)
import os
import struct
import subprocess
import sys

BG3_BIN = "/mnt/data/SteamLibrary/steamapps/common/Baldurs Gate 3/bin/bg3"

SITES = [
    ("predicate 0x37675f0", 0x37675F0,
     bytes.fromhex("41574156534883ec50"), bytes.fromhex("b801000000c3")),
    ("consumer branch 0x37677a3", 0x37677A3,
     bytes.fromhex("7543"), bytes.fromhex("9090")),
]


def first_load_vaddr(path):
    with open(path, "rb") as f:
        hdr = f.read(64)
        phoff, = struct.unpack_from("<Q", hdr, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", hdr, 0x36)
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ph = f.read(phentsize)
            p_type, = struct.unpack_from("<I", ph, 0)
            if p_type == 1:
                return struct.unpack_from("<Q", ph, 0x10)[0]
    raise RuntimeError("no PT_LOAD in %s" % path)


def mapped_base(pid):
    with open("/proc/%d/maps" % pid) as f:
        for line in f:
            parts = line.split(None, 5)
            if len(parts) == 6 and parts[5].strip().endswith("/bin/bg3"):
                return int(parts[0].split("-")[0], 16)
    raise RuntimeError("pid %d does not map %s" % (pid, BG3_BIN))


def bias_of(pid):
    return mapped_base(pid) - first_load_vaddr(BG3_BIN)


def bg3_pids():
    pids = [int(p) for p in subprocess.run(
        ["pgrep", "-x", "bg3"], capture_output=True, text=True).stdout.split()]
    if len(pids) != 2:
        sys.exit("expected 2 bg3 processes, found %d: %s" % (len(pids), pids))
    for p in pids:
        with open("/proc/%d/status" % p) as f:
            ppid = int([l for l in f if l.startswith("PPid:")][0].split()[1])
        if ppid in pids:
            return [ppid, p]
    sys.exit("neither bg3 process is the other's child: %s" % pids)


def read_mem(pid, addr, n):
    fd = os.open("/proc/%d/mem" % pid, os.O_RDONLY)
    try:
        return os.pread(fd, n, addr)
    finally:
        os.close(fd)


def read_gdb(pid, addr, n):
    cmd = ["sudo", "-n", "/usr/bin/gdb", "--batch", "-p", str(pid),
           "-ex", "set pagination off", "-ex", "x/%dxb 0x%x" % (n, addr)]
    out = subprocess.run(cmd, capture_output=True, text=True).stdout
    data = bytearray()
    for line in out.splitlines():
        tail = line.rsplit(":", 1)[-1].split()
        if tail and all(t.startswith("0x") for t in tail):
            data += bytes(int(t, 16) for t in tail)
    if len(data) < n:
        raise RuntimeError("gdb returned %d bytes:\n%s" % (len(data), out))
    return bytes(data[:n])


def classify(got, orig, patched):
    if got[:len(patched)] == patched:
        return "PATCHED"
    if got[:len(orig)] == orig:
        return "ORIGINAL"
    return "UNKNOWN"


def main():
    args = sys.argv[1:]
    use_gdb = "--gdb" in args
    pids = [int(a) for a in args if a != "--gdb"] or bg3_pids()
    reader = read_gdb if use_gdb else read_mem
    for pid in pids:
        with open("/proc/%d/status" % pid) as f:
            ppid = [l for l in f if l.startswith("PPid:")][0].split()[1]
        bias = bias_of(pid)
        print("pid %d (PPid %s) bias=0x%x" % (pid, ppid, bias))
        for name, vma, orig, patched in SITES:
            n = max(len(orig), len(patched))
            try:
                got = reader(pid, bias + vma, n)
            except PermissionError:
                sys.exit("cannot read /proc/%d/mem: run as root or with --gdb" % pid)
            print("  %-28s %-8s %s" % (name, classify(got, orig, patched), got.hex(" ")))


if __name__ == "__main__":
    main()
