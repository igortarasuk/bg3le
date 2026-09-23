#!/usr/bin/env bash
# Prints the bg3 parent and child PIDs.
# Usage: bg3_pids.sh [--parent|--child]
set -u

mapfile -t pids < <(pgrep -x bg3)
n=${#pids[@]}
if [ "$n" -eq 0 ]; then
    echo "bg3 is not running" >&2
    exit 1
fi
if [ "$n" -ne 2 ]; then
    echo "expected 2 bg3 processes, found $n: ${pids[*]}" >&2
    exit 1
fi

ppid_of() { awk '/^PPid:/ {print $2}' "/proc/$1/status"; }

parent=""; child=""
for p in "${pids[@]}"; do
    pp=$(ppid_of "$p")
    for q in "${pids[@]}"; do
        [ "$pp" = "$q" ] && { child=$p; parent=$q; }
    done
done
if [ -z "$child" ]; then
    echo "two bg3 processes but neither is the other's child: ${pids[*]}" >&2
    exit 1
fi

case "${1:-}" in
    --parent) echo "$parent" ;;
    --child)  echo "$child" ;;
    *)        echo "PARENT=$parent CHILD=$child" ;;
esac
