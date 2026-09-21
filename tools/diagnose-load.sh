#!/bin/bash
# Characterise the native build's slow save load, with NO extender attached.
# Answers: is it CPU-bound, I/O-bound, or blocked -- and on what.
OUT=/tmp/bg3-load-diagnosis.txt
SNIPER="$HOME/.local/share/Steam/steamapps/common/SteamLinuxRuntime_sniper"
: > "$OUT"

echo "launching the game WITHOUT the extender (control run)" | tee -a "$OUT"
( cd /home/lenon/bg3mods/bg3-linux-native && MANGOHUD=0 "$SNIPER/run" -- ./bin/bg3 >/dev/null 2>&1 ) &

# The wrapper and the game share a name; the game is the one with threads.
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
[ -z "$PID" ] && { echo "could not find the game process" | tee -a "$OUT"; exit 1; }
echo "game pid $PID ($BEST threads) -- load your save now" | tee -a "$OUT"

CLK=$(getconf CLK_TCK)
prev_cpu=0; prev_rd=0
for i in $(seq 1 300); do
    [ -d "/proc/$PID" ] || break
    set -- $(awk '{print $14, $15}' "/proc/$PID/stat" 2>/dev/null)
    cpu=$(( ${1:-0} + ${2:-0} ))
    rd=$(awk '/^read_bytes/{print $2}' "/proc/$PID/io" 2>/dev/null); rd=${rd:-0}
    nthr=$(awk '/^Threads/{print $2}' "/proc/$PID/status" 2>/dev/null)
    st=$(awk '{print $3}' "/proc/$PID/stat" 2>/dev/null)

    if [ "$prev_cpu" -gt 0 ]; then
        dcpu=$(( (cpu - prev_cpu) * 100 / CLK ))
        drd=$(( (rd - prev_rd) / 1048576 ))
        printf "%3ds cpu=%4d%% read=%5dMB/s thr=%-3s state=%s\n" \
            "$i" "$dcpu" "$drd" "$nthr" "$st" >> "$OUT"

        # While it is busy but not obviously computing, capture where it sits.
        if [ "$dcpu" -lt 150 ] && [ $(( i % 8 )) -eq 0 ]; then
            {
                echo "--- stacks at ${i}s (cpu=${dcpu}%) ---"
                timeout 20 eu-stack -p "$PID" 2>/dev/null | head -60
            } >> "$OUT"
        fi
    fi
    prev_cpu=$cpu; prev_rd=$rd
    sleep 1
done
echo "--- done, wrote $OUT"
