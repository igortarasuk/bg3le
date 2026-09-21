#!/bin/bash
# The process is blocked in futex_wait during the stall; capture the
# user-space call stacks so the waiting code can be identified.
OUT=/tmp/bg3-callstacks.txt
: > "$OUT"

PID=""
for i in $(seq 1 600); do
    BEST=0
    for p in $(pgrep -f "bin/bg3"); do
        n=$(awk '/^Threads/{print $2}' "/proc/$p/status" 2>/dev/null)
        [ -z "$n" ] && continue
        if [ "$n" -gt "$BEST" ]; then BEST=$n; PID=$p; fi
    done
    [ -n "$PID" ] && [ "$BEST" -gt 8 ] && break
    PID=""
    sleep 1
done
[ -z "$PID" ] && { echo "no game process" >> "$OUT"; exit 1; }
echo "game pid $PID -- load your save now" | tee -a "$OUT"

for i in $(seq 1 25); do
    [ -d "/proc/$PID" ] || break
    {
        echo "######## sample $i  $(date +%H:%M:%S) ########"
        timeout 25 eu-stack -p "$PID" 2>/dev/null
    } >> "$OUT"
    sleep 6
done
echo "--- done" >> "$OUT"
