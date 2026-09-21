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
runs is the text that ships. The internals it calls are stubbed over a plain
table, which also means the path syntax the view builds ("Field[0]") is
checked: the stub parses it back out, so a malformed path fails the test.

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

start = src.index("-- An array field is a view on the component, not a copy of it.")
end = src.index("-- A view over a set of fields, used for a component")
make_array = src[start:end]

harness = make_array + r"""
-- Stub: a 7-element backing store, zero-based as the engine is. The stub
-- parses the element path the view builds, so a malformed path shows up here.
local store = {[0]=0, [1]=17, [2]=13, [3]=15, [4]=8, [5]=12, [6]=10}
local count = 7

Ext = {_Internal = {}}

local function index_of(path)
  local base, i = path:match("^([%w_]+)%[(%d+)%]$")
  if base == nil then return nil, "unparseable path: " .. tostring(path) end
  return tonumber(i)
end

function Ext._Internal.ArrayInfo(handle, comp, path)
  return count, "int32"
end

function Ext._Internal.GetField(handle, comp, path)
  local i, err = index_of(path)
  if i == nil then return nil, err end
  if store[i] == nil then return nil, "index out of range" end
  return store[i]
end

function Ext._Internal.SetField(handle, comp, path, v)
  local i, err = index_of(path)
  if i == nil then return nil, err end
  if store[i] == nil then return nil, "index out of range" end
  store[i] = v
  return true
end

local a = make_array(1, "Stats", "Abilities")
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

local seen, last = 0, nil
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

-- The length is re-read rather than captured, so a dynamic array that grows
-- between accesses is seen at its new length.
count = 5
check("#a after shrink", #a, 5)
local shrunk = pcall(function() return a[7] end)
check("a[7] raises after shrink", shrunk, false)

if fails > 0 then
  print(fails .. " failure(s)")
  os.exit(1)
end
print("array view behaves")
"""

with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
    f.write(harness)
    path = f.name

try:
    sys.exit(subprocess.run(["lua", path]).returncode)
finally:
    os.unlink(path)
