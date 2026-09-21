#!/bin/bash
# Watch the game's TCP connections during a load, looking for something that
# sits pending for ~60s. Reads /proc only; no overhead on the game.
OUT=/tmp/bg3-net.txt
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

for i in $(seq 1 300); do
    [ -d "/proc/$PID" ] || break
    {
        printf "=== %3ds ===\n" "$i"
        # Non-loopback connections and anything not yet established.
        ss -tnp 2>/dev/null | grep -F "pid=$PID" \
            | grep -vE "127\.0\.0\.1" \
            | awk '{printf "  %-12s %-24s %s\n", $1, $4, $5}'
    } >> "$OUT"
    sleep 1
done
echo "--- done" >> "$OUT"
