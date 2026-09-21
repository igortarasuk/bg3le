#!/usr/bin/env python3
"""Checks the behaviour of the container views in lua_host.cpp's Lua prelude.

check-prelude.sh only parses the prelude; this runs parts of it. The array and
map views are the pieces with semantics worth testing rather than reading: they
present one-based Lua sequences and keyed tables over the engine's own
zero-based runs, and they have to write through to the component rather than
into a copy.

That last point is why this exists. The first array view returned a plain
table, so "component.Field[i] = v" read correctly and then silently discarded
the write -- no error and no effect, which is worse than refusing. A test that
only checked reads would have passed.

Each function is sliced out of lua_host.cpp rather than restated here, so what
runs is the text that ships. The internals they call are stubbed over plain
tables, which also checks the paths the views build: the stubs parse them back
out, so a malformed path fails the test.

The deeper walks -- a map of arrays of structs -- are covered by
bg3le_meta_selftest in src/vendor/component_meta.cpp instead, because those
need real memory rather than a stub. Run them with tools/meta-check.c.

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
    print("lua not found; skipping the view checks")
    sys.exit(0)

src = io.open(SOURCE, encoding="utf-8").read()


def slice_between(start_marker, end_marker):
    start = src.index(start_marker)
    end = src.index(end_marker)
    assert end > start, "markers out of order: " + start_marker
    return src[start:end]


make_array = slice_between(
    "-- An array field is a view on the component, not a copy of it.",
    "-- A map field is a view too, keyed the way the engine keys it.")

make_map = slice_between(
    "-- A map field is a view too, keyed the way the engine keys it.",
    "-- A view over a set of fields, used for a component")

PRELUDE = """
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
Ext = {_Internal = {}}
local make_fields
local make_map
"""

ARRAY_TEST = r"""
-- ---- the array view ----
--
-- A 7-element backing store, zero-based as the engine is. The stub parses the
-- element path the view builds, so a malformed path shows up here.
local store = {[0]=0, [1]=17, [2]=13, [3]=15, [4]=8, [5]=12, [6]=10}
local count = 7

local function index_of(path)
  local base, i = path:match("^([%w_]+)%[(%d+)%]$")
  if base == nil then return nil, "unparseable path: " .. tostring(path) end
  return tonumber(i)
end

function Ext._Internal.ArrayInfo(handle, comp, path) return count, "int32" end

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

check("a[1]", a[1], 0)
check("a[2]", a[2], 17)
check("a[7]", a[7], 10)

-- The whole point: an element write has to reach the store.
a[2] = 99
check("a[2] after write", a[2], 99)
check("store[1] after write", store[1], 99)

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

for _, bad in ipairs({0, 8, -1}) do
  check("read a[" .. bad .. "] raises",
        pcall(function() return a[bad] end), false)
  check("write a[" .. bad .. "] raises",
        pcall(function() a[bad] = 1 end), false)
end
check("write a.nope raises", pcall(function() a.nope = 1 end), false)

-- The length is re-read rather than captured, so a dynamic array that changes
-- between accesses is seen at its new length.
count = 5
check("#a after shrink", #a, 5)
check("a[7] raises after shrink", pcall(function() return a[7] end), false)
"""

MAP_TEST = r"""
-- ---- the map view ----
--
-- Two parallel runs, as the engine stores them: slot i holds key i and value
-- i. Values here are scalars, which keeps the stub simple.
local keys = {[0]="alpha", [1]="beta", [2]="gamma"}
local values = {[0]=10, [1]=20, [2]=30}
local mapCount = 3
local keysConvertible = true

function Ext._Internal.ArrayInfo(handle, comp, path)
  return mapCount, "int32"
end

function Ext._Internal.MapKey(handle, comp, path, i)
  if not keysConvertible then return nil, "unsupported key kind" end
  if keys[i] == nil then return nil, "no key at that slot" end
  return keys[i]
end

function Ext._Internal.FieldInfo(comp, path) return "int32", "", 0 end

function Ext._Internal.GetField(handle, comp, path)
  local base, i = path:match("^([%w_]+)%[(%d+)%]$")
  if base == nil then return nil, "unparseable path: " .. tostring(path) end
  return values[tonumber(i)]
end

function Ext._Internal.SetField(handle, comp, path, v)
  local base, i = path:match("^([%w_]+)%[(%d+)%]$")
  if base == nil then return nil, "unparseable path: " .. tostring(path) end
  values[tonumber(i)] = v
  return true
end

local m = make_map(1, "ActionResources", "Resources")

-- Lookup is by key, and linear, though that is not observable from here.
check("m.alpha", m["alpha"], 10)
check("m.gamma", m["gamma"], 30)
check("m of a missing key", m["nope"], nil)
check("#m", #m, 3)

-- A write goes to the slot the key occupies.
m["beta"] = 99
check("m.beta after write", m["beta"], 99)
check("values[1] after write", values[1], 99)

-- Adding a key is refused rather than silently dropped.
check("adding a key raises", pcall(function() m["delta"] = 1 end), false)

-- Iteration yields key, value pairs.
local pairsSeen, total = 0, 0
for k, v in pairs(m) do
  pairsSeen = pairsSeen + 1
  total = total + v
  if type(k) ~= "string" then
    print("FAIL map pairs yielded a non-string key: " .. tostring(k))
    fails = fails + 1
  end
end
check("map pairs count", pairsSeen, 3)
check("map pairs value total", total, 10 + 99 + 30)

-- Entries() walks by slot, so it works even when keys cannot be converted.
check("Entries count", #m.Entries(), 3)
check("Entries[1].Key", m.Entries()[1].Key, "alpha")
check("Entries[1].Value", m.Entries()[1].Value, 10)

keysConvertible = false
check("pairs stops when keys cannot convert", (function()
  local n = 0
  for _ in pairs(m) do n = n + 1 end
  return n
end)(), 0)
check("Entries still returns the values", #m.Entries(), 3)
check("Entries[2].Value without keys", m.Entries()[2].Value, 99)
keysConvertible = true

if fails > 0 then
  print(fails .. " failure(s)")
  os.exit(1)
end
print("array and map views behave")
"""

harness = PRELUDE + make_array + make_map + ARRAY_TEST + MAP_TEST

with tempfile.NamedTemporaryFile("w", suffix=".lua", delete=False) as f:
    f.write(harness)
    path = f.name

try:
    sys.exit(subprocess.run(["lua", path]).returncode)
finally:
    os.unlink(path)
