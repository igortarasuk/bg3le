# Generates a one-shot gdb Python script that attaches to a running bg3
# process, arms a non-interrupting entry breakpoint at a given raw-VMA
# address (bias applied automatically), logs rdi/rsi on every hit, and
# auto-detaches after a timeout. Non-interrupting: the breakpoint's stop()
# returns False, so the game keeps running -- no gameplay pause.
#
# IMPORTANT (found 2026-09-23, Session 7): bg3 runs as TWO OS processes, a
# parent and a child (`pgrep -x bg3` shows both; `cat /proc/<pid>/status |
# grep PPid` tells you which is which). Both share the SAME ASLR bias for
# the main "bg3" executable (consistent with the child being a plain
# fork(), not a fresh exec -- confirmed by reading /proc/<pid>/maps for
# both and comparing the mapped base address). Despite the PARENT's own
# bg3le.log.<pid> containing ALL the visible Osiris logging (RegisterDIV-
# Functions, StoryLoaded, COsiris::Event, the Steam vtable-hook install
# line), the actual esv::gEocServer / Osiris-native-handler code (e.g. the
# achievement UnlockAchievement handler at raw VMA 0x3767780) executes in
# the CHILD, not the parent -- confirmed by breakpointing both
# simultaneously: zero hits in the parent, 8 hits in the child for the same
# test. There is no separate bg3le.log.<child-pid> (the child never runs
# its own copy of bg3le's constructor/log_init -- it inherited the
# already-initialized state via fork), so anything you need to observe from
# the child MUST be done live (breakpoints, direct /proc/<pid>/mem reads),
# not via its own log file.
#
# Usage: python3 entry_watch_gen.py <pid> <out_log_path> <tag> [hex_addr]
#   hex_addr defaults to 0x3767780 (the UnlockAchievement handler entry)
#   if omitted -- pass e.g. 0x398e8e0 to watch a different raw VMA.
#
# Run the generated script directly with gdb (sudo -n /usr/bin/gdb -batch
# -x <tag>.gdb) -- NOT wrapped in env/timeout/a shell script, or the
# NOPASSWD sudoers rule (which matches only the literal `gdb` command) will
# prompt for a password. See ACHIEVEMENTS-DIAGNOSIS.md "Session 6"/"Session
# 7" for the full sudo/gdb setup notes.
import sys

pid = sys.argv[1]
out = sys.argv[2]
tag = sys.argv[3]
addr = sys.argv[4] if len(sys.argv) > 4 else "0x3767780"

script = f'''python
import gdb
import threading

PID = {pid}

gdb.execute("set logging file {out}")
gdb.execute("set logging overwrite on")
gdb.execute("set logging enabled on")
gdb.execute("set pagination off")
gdb.execute("attach %d" % PID)

bias = None
with open("/proc/%d/maps" % PID) as f:
    for line in f:
        parts = line.strip().split(None, 5)
        if len(parts) < 6:
            continue
        if parts[5].endswith("/bg3"):
            bias = int(parts[0].split("-")[0], 16)
            break
print("[{tag}] bias = 0x%x" % bias)

entry_addr = bias + {addr}

class EntryBP(gdb.Breakpoint):
    def stop(self):
        try:
            rdi = int(gdb.parse_and_eval("$rdi"))
            rsi = int(gdb.parse_and_eval("$rsi"))
            print("[{tag}] ENTRY HIT: rdi=0x%x rsi=0x%x" % (rdi, rsi))
        except Exception as e:
            print("[{tag}] ENTRY HIT: error %s" % e)
        return False

EntryBP("*0x%x" % entry_addr)

def do_detach():
    try:
        print("[{tag}] TIMEOUT: auto-detaching now")
        gdb.execute("detach")
    except Exception as e:
        print("[{tag}] auto-detach failed: %s" % e)

timer = threading.Timer(1200.0, lambda: gdb.post_event(do_detach))
timer.daemon = True
timer.start()

print("[{tag}] Breakpoint armed, continuing for up to 1200s.")
gdb.execute("continue")
end
'''
with open(f"{tag}.gdb", "w") as f:
    f.write(script)
print(f"wrote {tag}.gdb (PID={pid}, addr={addr}, log={out})")
