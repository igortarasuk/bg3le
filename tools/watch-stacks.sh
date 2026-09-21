#!/bin/bash
# Dump every thread's stack repeatedly during the load. The wait is idle, so
# whatever the game is blocked on will be sitting in these stacks.
OUT=/tmp/bg3-stacks.txt
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

for i in $(seq 1 40); do
    [ -d "/proc/$PID" ] || break
    {
        echo "######## sample $i ########"
        # Per-thread kernel wait channel: cheap, and names the blocking call.
        for t in /proc/$PID/task/*; do
            comm=$(cat "$t/comm" 2>/dev/null)
            st=$(awk '{print $3}' "$t/stat" 2>/dev/null)
            wc=$(cat "$t/wchan" 2>/dev/null)
            sys=$(awk '{print $1}' "$t/syscall" 2>/dev/null)
            [ "$st" = "S" ] || [ "$st" = "D" ] && printf "  %-18s %s syscall=%-4s wchan=%s\n" "$comm" "$st" "$sys" "$wc"
        done | sort | uniq -c | sort -rn | head -14
    } >> "$OUT"
    sleep 4
done
echo "--- done" >> "$OUT"
