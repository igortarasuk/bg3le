#!/bin/bash
# Syntax-checks the Lua embedded in src/lua_host.cpp.
#
# The prelude is a raw string literal, so the C++ compiler will happily accept
# anything inside it: a syntax error there is not a build failure but a
# runtime one, and it takes out every Ext.* function at once. luac -p parses
# without executing, which catches it in a second.
#
# Needs luac on PATH. The dialect is close enough for parsing -- the embedded
# code uses no 5.3-vs-5.4 syntax differences -- so the system one will do even
# though the game runs Norbyte's fork.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../src/lua_host.cpp"

if ! command -v luac >/dev/null; then
    echo "luac not found; skipping the prelude syntax check" >&2
    exit 0
fi

# Every R"LUA( ... )LUA" block, each checked on its own so a reported line
# number means something.
python3 - "$SRC" <<'PY'
import re
import subprocess
import sys
import tempfile

path = sys.argv[1]
src = open(path, encoding="utf-8").read()

blocks = re.findall(r'R"LUA\(\n(.*?)\n\)LUA"', src, re.DOTALL)
if not blocks:
    print("no embedded Lua blocks found -- has the delimiter changed?")
    sys.exit(1)

# Line numbers so a failure points into lua_host.cpp rather than into a
# temporary file.
offsets = [src[:m.start()].count("\n") + 2
           for m in re.finditer(r'R"LUA\(\n', src)]

failed = 0
for i, (block, offset) in enumerate(zip(blocks, offsets), 1):
    with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
        f.write(block)
        name = f.name
    result = subprocess.run(["luac", "-p", name], capture_output=True, text=True)
    if result.returncode == 0:
        print(f"block {i} ({len(block.splitlines())} lines "
              f"at lua_host.cpp:{offset}): ok")
    else:
        failed += 1
        message = (result.stderr or result.stdout).strip()
        # luac reports "file:line:", which is a temporary path and a line
        # relative to the block; rewrite both to point at the real source.
        def fix(m):
            return f"{path}:{int(m.group(1)) + offset - 1}:"
        print(re.sub(rf"{re.escape(name)}:(\d+):", fix, message))

sys.exit(1 if failed else 0)
PY

echo "prelude syntax ok"
