#!/bin/bash
# Profile a frametime hitch. Run this, then trigger a cutscene.
# Looks for shader-compiler symbols, which would confirm pipeline compilation.
OUT=/tmp/bg3-hitch.perf.data
PID=""
for i in $(seq 1 120); do
    BEST=0
    for p in $(pgrep -f "bin/bg3|ShadowOfMordor|shadowofmordor"); do
        n=$(awk '/^Threads/{print $2}' "/proc/$p/status" 2>/dev/null)
        [ -z "$n" ] && continue
        if [ "$n" -gt "$BEST" ]; then BEST=$n; PID=$p; fi
    done
    [ -n "$PID" ] && [ "$BEST" -gt 8 ] && break
    PID=""; sleep 1
done
[ -z "$PID" ] && { echo "no game process found"; exit 1; }
echo "profiling pid $PID for 40s -- TRIGGER A CUTSCENE NOW"
perf record -F 299 -g -p "$PID" -o "$OUT" -- sleep 40 2>&1 | tail -2
echo "--- done: $OUT"
