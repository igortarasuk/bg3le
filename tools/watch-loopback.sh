#!/bin/bash
# Sample loopback traffic during a save load. Zero overhead -- reads kernel
# counters, does not touch the game.
OUT=/tmp/bg3-loopback2.txt
: > "$OUT"
echo "watching lo; load your save now" | tee -a "$OUT"

prx=0; ppk=0
for i in $(seq 1 420); do
    rx=$(cat /sys/class/net/lo/statistics/rx_bytes)
    pk=$(cat /sys/class/net/lo/statistics/rx_packets)
    if [ "$prx" -gt 0 ]; then
        d=$(( (rx - prx) / 1024 ))
        p=$(( pk - ppk ))
        avg=0; [ "$p" -gt 0 ] && avg=$(( (rx - prx) / p ))
        printf "%3ds  %7d KB/s  %7d pkt/s  avg %5d B/pkt\n" "$i" "$d" "$p" "$avg" >> "$OUT"
    fi
    prx=$rx; ppk=$pk
    sleep 1
done
echo "--- done" >> "$OUT"
