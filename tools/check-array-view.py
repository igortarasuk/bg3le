#!/usr/bin/env python3
"""Checks the behaviour of the array view in lua_host.cpp's Lua prelude.

check-prelude.sh only parses the prelude; this runs part of it. The array view
is the piece with semantics worth testing rather than reading: it presents a
one-based Lua array over a zero-based component field, and it has to write
through to the component rather than into a copy.

That last point is why this exists. The first version returned a plain table,
so "component.Field[i] = v" read correctly and then silently discarded the
write -- no error and no effect, which is worse than refusing. A test that
only checked reads would have passed.

The function is sliced out of lua_host.cpp rather than restated here, so what
runs is the text that ships. GetElement and SetElement are stubbed with a
plain table, so this needs neither the game nor the built library.

Needs lua on PATH.
"""
import io
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "src", "lua_host.cpp")

if shutil.which("lua") is None:
    print("lua not found; skipping the array view check")
    sys.exit(0)

src = io.open(SOURCE, encoding="utf-8").read()

start = src.index("local function make_array(")
end = src.index("local function make_component(")
make_array = src[start:end]

harness = (
    make_array
    + r"""
-- Stub: a 7-element backing store, zero-based as the engine is.
local store = {[0]=0, [1]=17, [2]=13, [3]=15, [4]=8, [5]=12, [6]=10}
local reads, writes = 0, 0

Ext = {_Internal = {}}
function Ext._Internal.GetElement(handle, comp, field, i)
  reads = reads + 1
  if store[i] == nil then return nil, "index out of range" end
  return store[i]
end
function Ext._Internal.SetElement(handle, comp, field, i, v)
  writes = writes + 1
  if store[i] == nil then return nil, "index out of range" end
  store[i] = v
  return true
end

local a = make_array(1, "Stats", "Abilities", 7)
local fails = 0
local function check(what, got, want)
  if got ~= want then
    print(string.format("FAIL %s: got %s, want %s", what, tostring(got),
                        tostring(want)))
    fails = fails + 1
  else
    print(string.format("ok   %s = %s", what, tostring(got)))
  end
end

-- One-based reads map onto the zero-based store.
check("a[1]", a[1], 0)
check("a[2]", a[2], 17)
check("a[7]", a[7], 10)

-- The whole point: an element write has to reach the store.
a[2] = 99
check("a[2] after write", a[2], 99)
check("store[1] after write", store[1], 99)

-- Length and iteration, which the JSON serializer relies on.
check("#a", #a, 7)

local seen = 0
local last
for i, v in pairs(a) do
  seen = seen + 1
  last = i
  if type(i) ~= "number" then
    print("FAIL pairs yielded a non-numeric key: " .. tostring(i))
    fails = fails + 1
  end
end
check("pairs count", seen, 7)
check("pairs last index", last, 7)

-- Out of range must raise, not read as nil, at both ends.
for _, bad in ipairs({0, 8, -1}) do
  local ok = pcall(function() return a[bad] end)
  check("read a[" .. bad .. "] raises", ok, false)
  local ok2 = pcall(function() a[bad] = 1 end)
  check("write a[" .. bad .. "] raises", ok2, false)
end

-- A non-numeric key must raise too rather than silently doing nothing.
local ok = pcall(function() a.nope = 1 end)
check("write a.nope raises", ok, false)

if fails > 0 then
  print(fails .. " failure(s)")
  os.exit(1)
end
print("array view behaves")
"""
)

with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
    f.write(harness)
    path = f.name

try:
    sys.exit(subprocess.run(["lua", path]).returncode)
finally:
    os.unlink(path)
