#!/usr/bin/env python3
# Generates a gdb script: breakpoint on SetAchievement, log name + bt.
# Usage: steam_bt_gen.py <pid> <hex_addr> <out_log> <tag> [timeout_s] [--raw]
import sys

BG3_BIN = "/mnt/data/SteamLibrary/steamapps/common/Baldurs Gate 3/bin/bg3"

args = [a for a in sys.argv[1:] if a != "--raw"]
if len(args) < 4:
    sys.exit(__doc__ or "usage: steam_bt_gen.py <pid> <hex_addr> <out_log> <tag> [timeout_s] [--raw]")
pid, addr, out, tag = args[:4]
timeout = float(args[4]) if len(args) > 4 else 600.0
raw = "--raw" in sys.argv

script = f'''python
import gdb
import struct
import threading

PID = {pid}
TAG = "{tag}"
RAW = {raw!r}
BG3_BIN = "{BG3_BIN}"

gdb.execute("set logging file {out}")
gdb.execute("set logging overwrite on")
gdb.execute("set logging enabled on")
gdb.execute("set pagination off")
gdb.execute("set print frame-arguments none")
gdb.execute("set backtrace limit 128")
gdb.execute("attach %d" % PID)

def first_load_vaddr(path):
    with open(path, "rb") as f:
        hdr = f.read(64)
        phoff, = struct.unpack_from("<Q", hdr, 0x20)
        phentsize, phnum = struct.unpack_from("<HH", hdr, 0x36)
        for i in range(phnum):
            f.seek(phoff + i * phentsize)
            ph = f.read(phentsize)
            if struct.unpack_from("<I", ph, 0)[0] == 1:
                return struct.unpack_from("<Q", ph, 0x10)[0]
    raise RuntimeError("no PT_LOAD")

def mapped_base():
    with open("/proc/%d/maps" % PID) as f:
        for line in f:
            parts = line.split(None, 5)
            if len(parts) == 6 and parts[5].strip().endswith("/bin/bg3"):
                return int(parts[0].split("-")[0], 16)
    raise RuntimeError("bg3 not mapped")

bias = mapped_base() - first_load_vaddr(BG3_BIN)
target = {addr} + (bias if RAW else 0)
print("[%s] bias=0x%x breakpoint at 0x%x" % (TAG, bias, target))

class SteamBP(gdb.Breakpoint):
    hits = 0
    def stop(self):
        SteamBP.hits += 1
        try:
            th = gdb.selected_thread()
            tid = th.ptid[1] if th is not None else -1
            try:
                name = gdb.parse_and_eval("(const char*)$rsi").string()
            except Exception as e:
                name = "<unreadable rsi: %s>" % e
            print("[%s] HIT #%d thread lwp=%d rdi(this)=0x%x name=%s" % (
                TAG, SteamBP.hits, tid, int(gdb.parse_and_eval("$rdi")), name))
            print(gdb.execute("bt", to_string=True))
        except Exception as e:
            print("[%s] HIT #%d: error %s" % (TAG, SteamBP.hits, e))
        return False

SteamBP("*0x%x" % target)

def do_detach():
    try:
        print("[%s] TIMEOUT after {timeout}s, %d hits, detaching" % (TAG, SteamBP.hits))
        gdb.execute("detach")
    except Exception as e:
        print("[%s] detach failed: %s" % (TAG, e))

timer = threading.Timer({timeout}, lambda: gdb.post_event(do_detach))
timer.daemon = True
timer.start()

print("[%s] armed, continuing for up to {timeout}s" % TAG)
gdb.execute("continue")
end
'''
path = f"{tag}.gdb"
with open(path, "w") as f:
    f.write(script)
print(f"wrote {path} (PID={pid}, addr={addr}{' raw+bias' if raw else ' absolute'}, "
      f"log={out}, timeout={timeout:g}s)")
print(f"run: sudo -n /usr/bin/gdb -batch -x {path}")
