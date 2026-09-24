#!/bin/bash
# Runs every captured reference query against bg3le and diffs the answers.
#
# reference/*.txt is output captured from the real Script Extender on Windows
# (tools/grab-reference.sh took it). Everything bg3le reimplements has to match
# those in shape, or a mod written against upstream will not work here -- but
# nothing was comparing them, so a divergence could only be noticed by reading.
#
# This does not decide pass or fail, and that is deliberate. Some captures are
# environment-specific by nature: the host entity depends on the save, the
# version depends on the build, an entity count depends on what is loaded. A
# harness that called those failures would cry wolf until it was ignored. So it
# reports how far apart each pair is and leaves the judgement to a person,
# with the differing lines a diff away.
#
# Needs the game running with bg3le attached. The queries come from
# grab-reference.sh itself, parsed out of it, so the two cannot drift.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$HERE/.."
REF="$ROOT/reference"
CLI="$ROOT/client/bg3lua"
OUT="${BG3LE_REFOUT:-$REF/bg3le}"

if [ ! -x "$CLI" ]; then
    echo "check-reference: $CLI not found" >&2
    exit 1
fi

mkdir -p "$OUT"

# The name and the Lua of every captured query, read out of the capture script
# rather than restated. A `grab NAME 'code'` call, where the code may span
# lines, is what is being matched.
#
# Written to files rather than passed in a variable: the Lua contains newlines,
# and bash strips NUL bytes out of a command substitution, so there is no
# separator that survives both.
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - "$HERE/grab-reference.sh" "$work" <<'PYEOF'
import os
import re
import sys

text = open(sys.argv[1], encoding="utf-8").read()
out = sys.argv[2]
names = []
for m in re.finditer(r"^grab\s+(\S+)\s+'(.*?)'\s*$", text, re.S | re.M):
    name, code = m.group(1), m.group(2)
    names.append(name)
    with open(os.path.join(out, name + ".lua"), "w", encoding="utf-8") as f:
        f.write(code)

with open(os.path.join(out, "names"), "w", encoding="utf-8") as f:
    f.write("\n".join(names) + "\n")
PYEOF

total=0
same=0
differ=0
ordering=0
failed=0

while IFS= read -r name; do
    [ -n "$name" ] || continue

    total=$((total + 1))
    printf '  %-34s ' "$name"

    if [ ! -f "$REF/$name.txt" ]; then
        printf 'no capture to compare against\n'
        continue
    fi

    if ! timeout 120 "$CLI" -f "$work/$name.lua" > "$OUT/$name.txt" 2>&1; then
        printf 'bg3le: the query failed\n'
        failed=$((failed + 1))
        continue
    fi

    # The console prints its banner and the extender's startup lines before the
    # answer; the capture from upstream does not. Compare only what the query
    # itself produced, which is everything after the last startup line.
    sed -i '1,/^story: bg3le used/d' "$OUT/$name.txt" 2>/dev/null || true

    # A pointer printed into a dump is never going to match: upstream's are
    # Windows addresses and ours are this process's. Both sides are normalised
    # before comparing so the noise does not drown the signal -- the thing
    # worth knowing is that the key is a function, not where it lives.
    norm_ref="$work/$name.ref"
    norm_out="$work/$name.out"
    sed -E 's/(function|table|userdata): [0-9A-Fa-fx]+/\1: <address>/g' \
        "$REF/$name.txt" > "$norm_ref"
    sed -E 's/(function|table|userdata): [0-9A-Fa-fx]+/\1: <address>/g' \
        "$OUT/$name.txt" > "$norm_out"

    if diff -q "$norm_ref" "$norm_out" > /dev/null 2>&1; then
        printf 'identical\n'
        same=$((same + 1))
    else
        lines="$(diff "$norm_ref" "$norm_out" | grep -c '^[<>]' || true)"
        ref="$(wc -l < "$norm_ref")"

        # And again with both sides sorted. A dump's key order is whatever
        # pairs() happened to give -- Lua does not define it and a mod cannot
        # depend on it -- so a difference that disappears under sort is a
        # difference in order, not in content, and saying which saves the
        # next reader the diff.
        # Sorted, and with the trailing commas off: a JSON list whose
        # members are the same but in a different order differs only in
        # where the commas fall, and that is an order difference too.
        sorted_lines="$(diff <(sed 's/,$//' "$norm_ref" | sort) \
                             <(sed 's/,$//' "$norm_out" | sort) \
                        | grep -c '^[<>]' || true)"
        if [ "$sorted_lines" = "0" ]; then
            printf '%s of %s lines differ, but only in order\n' "$lines" "$ref"
            ordering=$((ordering + 1))
        else
            printf '%s of %s lines differ (%s of them in content)\n' \
                   "$lines" "$ref" "$sorted_lines"
            differ=$((differ + 1))
        fi
    fi
done < "$work/names"

echo
echo "$total queries: $same identical, $ordering differ only in order," \
     "$differ differ in content, $failed failed"
echo "Addresses are normalised; identical means identical apart from those."
echo "bg3le's answers are in $OUT; diff them against $REF"
