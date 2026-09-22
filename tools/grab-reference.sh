#!/bin/bash
# Capture reference output from the real Script Extender, for comparison.
#
# Everything bg3le reimplements has to match these byte for byte in shape,
# or mods written against upstream will not work against it. Each file is
# one query so a diff points at one thing.
set -u
REF=/home/lenon/bg3mods/bg3le/reference
CLI="/home/lenon/bg3mods/bg3lua/bg3lua"
mkdir -p "$REF"

grab() {
    local name="$1"; shift
    local code="$1"
    printf '  %-34s ' "$name"
    if timeout 90 "$CLI" -e "$code" > "$REF/$name.txt" 2>&1; then
        printf 'ok (%s bytes)\n' "$(stat -c%s "$REF/$name.txt")"
    else
        printf 'FAILED\n'
    fi
}

echo "== stats: one per modifier list =="
grab stats-weapon        '_D(Ext.Stats.Get("WPN_Longsword"))'
grab stats-spell         '_D(Ext.Stats.Get("Target_MainHandAttack"))'
grab stats-status        '_D(Ext.Stats.Get("BURNING"))'
grab stats-base-weapon   '_D(Ext.Stats.Get("_BaseWeapon"))'

echo "== stats: the shape of the manager =="
grab stats-modifier-lists '
local m = Ext.Stats.GetStatsManager()
local out = {}
for i, l in ipairs(m.ModifierLists) do out[#out+1] = i .. ": " .. tostring(l.Name) end
print(table.concat(out, "\n"))'
grab stats-count         'print(#Ext.Stats.GetStats())'
grab stats-names-sample  '
local s = Ext.Stats.GetStats()
for i = 1, 20 do print(s[i]) end'

echo "== stats: attribute metadata, which drives our decoding =="
grab stats-modifier-attrs-weapon '_D(Ext.Stats.GetModifierAttributes("Weapon"))'
grab stats-enum-roundtrip '
print(Ext.Stats.EnumLabelToIndex("Damage Type", "Slashing"))
print(Ext.Stats.EnumIndexToLabel("Damage Type", 3))'

echo "== static data, which bg3le already implements =="
grab staticdata-actionresource '
local all = Ext.StaticData.GetAll("ActionResource")
print("count: " .. #all)
local r = Ext.StaticData.Get("d6b2369d-84f0-4ca4-a3a7-62d2d192a185", "ActionResource")
_D(r)'
grab staticdata-types '
local m = Ext.StaticData.GetSources and "has GetSources" or "no GetSources"
print(m)
for _, t in ipairs(Ext.Enums.ExtResourceManagerType) do print(t) end'

echo "== ecs, which bg3le already implements =="
grab entity-component-types '
local t = Ext.Entity.GetRegisteredComponentTypes()
print("count: " .. #t)
for i = 1, 40 do print(t[i]) end'

echo "== versions, so a future mismatch is attributable =="
grab version '
print("GameVersion: " .. tostring(Ext.Utils.GameVersion()))
_D(Ext.Utils.Version())'
