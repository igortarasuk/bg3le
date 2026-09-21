#!/bin/bash
# Both CPU and GPU are idle during the hitch, so something is waiting.
# Sample per-thread wait state at 10Hz, plus disk I/O, to find out what.
OUT=/tmp/bg3-frames.txt
: > "$OUT"

PID=""
for i in $(seq 1 120); do
    BEST=0
    for p in $(pgrep -f "bin/bg3"); do
        n=$(awk '/^Threads/{print $2}' "/proc/$p/status" 2>/dev/null)
        [ -z "$n" ] && continue
        if [ "$n" -gt "$BEST" ]; then BEST=$n; PID=$p; fi
    done
    [ -n "$PID" ] && [ "$BEST" -gt 8 ] && break
    PID=""; sleep 1
done
[ -z "$PID" ] && { echo "no game process" >> "$OUT"; exit 1; }
echo "pid $PID -- trigger a cutscene" | tee -a "$OUT"

prev_rd=0
for i in $(seq 1 600); do
    [ -d "/proc/$PID" ] || break
    rd=$(awk '/^read_bytes/{print $2}' "/proc/$PID/io" 2>/dev/null); rd=${rd:-0}
    drd=$(( (rd - prev_rd) / 1024 ))
    prev_rd=$rd

    # Only the threads that drive frames; the worker pool is noise here.
    {
        printf "t=%-6.1f readKB=%-7d " "$(echo "$i/10" | bc -l)" "$drd"
        for t in /proc/$PID/task/*; do
            c=$(cat "$t/comm" 2>/dev/null)
            case "$c" in
                GameThread|bg3|"WSI swapchain q"|RenderThread|"GPUDevice::Dele")
                    printf "[%s %s/%s] " "$c" "$(awk '{print $3}' "$t/stat" 2>/dev/null)" \
                        "$(cat "$t/wchan" 2>/dev/null | cut -c1-18)" ;;
            esac
        done
        echo
    } >> "$OUT"
    sleep 0.1
done
echo "--- done" >> "$OUT"
