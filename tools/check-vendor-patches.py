#!/usr/bin/env python3
"""Verifies the clang fixes listed in vendor/NOTICE.md are still applied.

Re-copying files from an upstream bg3se checkout silently reverts them, which
has already happened twice. Run this after any such copy.
"""
import pathlib
import re
import sys

V = pathlib.Path(__file__).resolve().parent.parent / "vendor" / "bg3se"
SOURCES = [p for p in V.rglob("*") if p.is_file() and p.suffix in (".h", ".inl", ".cpp")]


def text(rel):
    return (V / rel).read_text(errors="replace")


def grep_count(pattern, flags=0):
    rx = re.compile(pattern, flags)
    return sum(1 for p in SOURCES if rx.search(p.read_text(errors="replace")))


CHECKS = [
    ("requires-clause wrapped in parens",
     lambda: grep_count(r"requires !") == 0),
    ("no stray typename before a builtin type",
     lambda: grep_count(r"^typename (bool|void|int|float|double|char) ", re.M) == 0),
    ("FunctionImpl friend declaration names its parameters",
     lambda: "template <class TFun, class TData> friend class FunctionImpl;"
             in text("CoreLib/Base/BaseFunction.h")),
    ("__FUNCTION__ concatenation removed",
     lambda: '__FUNCTION__ "(): "' not in text("BG3Extender/Extender/Shared/Utils.h")),
    ("std::thread included rather than forward-declared",
     lambda: "#include <thread>" in text("BG3Extender/Extender/Shared/Utils.h")),
    ("SDL_HOOK specialisations marked template<>",
     lambda: "template<> SDL##name##HookType"
             in text("BG3Extender/Extender/Client/SDLManager.h")),
    ("LuaStats.h include uses the real directory case",
     lambda: "<Lua/LuaBinding.h>" in text("BG3Extender/Lua/Shared/LuaStats.h")),
    ("BuildInfo.h transcoded to UTF-8",
     lambda: (V / "BG3Extender/Extender/BuildInfo.h").read_bytes()[:2] != b"\xff\xfe"),
    ("Config.h transcoded to UTF-8",
     lambda: (V / "CoreLib/Config.h").read_bytes()[:2] != b"\xff\xfe"),
    ("log macros use __VA_OPT__",
     lambda: "__VA_OPT__" in text("CoreLib/Utils.h")),
    ("FixedStringUnhashed has a stream operator",
     lambda: "bg3se::FixedStringUnhashed const& str" in text("CoreLib/Base/BaseString.h")),
]


def main():
    failed = 0
    for name, check in CHECKS:
        try:
            ok = check()
        except Exception as exc:  # a missing file is a failure, not a crash
            print(f"  ERR  {name}: {exc}")
            failed += 1
            continue
        print(f"  {'OK  ' if ok else 'FAIL'} {name}")
        failed += not ok
    if failed:
        print(f"\n{failed} vendored fix(es) missing; see vendor/NOTICE.md")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
