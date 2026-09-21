#!/bin/bash
# Sample GPU-side state during a frametime hitch. The CPU profile is flat, so
# whatever stalls frames is either GPU work, clock/power ramping, or memory
# traffic. Reads sysfs only; no overhead on the game.
OUT=/tmp/bg3-gpu.txt
: > "$OUT"

# Pick the discrete card (the one with the most VRAM).
CARD=""
BEST=0
for d in /sys/class/drm/card*/device; do
    [ -f "$d/mem_info_vram_total" ] || continue
    t=$(cat "$d/mem_info_vram_total" 2>/dev/null)
    [ -z "$t" ] && continue
    if [ "$t" -gt "$BEST" ]; then BEST=$t; CARD=$d; fi
done
[ -z "$CARD" ] && { echo "no amdgpu card found" >> "$OUT"; exit 1; }
echo "card: $CARD  vram_total: $((BEST/1048576)) MiB" | tee -a "$OUT"
echo "sampling 60s at 10Hz -- trigger a cutscene" | tee -a "$OUT"

printf "%8s %6s %8s %8s %9s %7s\n" time busy% vram_MiB gtt_MiB sclk_MHz mclk_MHz >> "$OUT"
for i in $(seq 1 600); do
    busy=$(cat "$CARD/gpu_busy_percent" 2>/dev/null)
    vram=$(( $(cat "$CARD/mem_info_vram_used" 2>/dev/null || echo 0) / 1048576 ))
    gtt=$(( $(cat "$CARD/mem_info_gtt_used" 2>/dev/null || echo 0) / 1048576 ))
    sclk=$(awk '/\*/{gsub(/Mhz|MHz/,"");print $2}' "$CARD/pp_dpm_sclk" 2>/dev/null | head -1)
    mclk=$(awk '/\*/{gsub(/Mhz|MHz/,"");print $2}' "$CARD/pp_dpm_mclk" 2>/dev/null | head -1)
    printf "%7.1fs %6s %8s %8s %9s %7s\n" "$(echo "$i/10" | bc -l)" \
        "${busy:-?}" "$vram" "$gtt" "${sclk:-?}" "${mclk:-?}" >> "$OUT"
    sleep 0.1
done
echo "--- done" >> "$OUT"
