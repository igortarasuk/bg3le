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
#!/bin/bash
# Second reference pass: the modules bg3le has still to implement, plus the
# entity and component shapes it already partly does.
set -u
REF=/home/lenon/bg3mods/bg3le/reference
CLI="/home/lenon/bg3mods/bg3lua/bg3lua"

grab() {
    local name="$1"; shift
    printf '  %-34s ' "$name"
    if timeout 90 "$CLI" -e "$1" > "$REF/$name.txt" 2>&1; then
        printf 'ok (%s bytes)\n' "$(stat -c%s "$REF/$name.txt")"
    else
        printf 'FAILED\n'
    fi
}

echo "== entity and components, which bg3le already reads =="
grab entity-host '
local e = Ext.Entity.GetAllEntitiesWithComponent("Uuid")
print("entities with Uuid: " .. #e)
local host = nil
for _, ent in ipairs(e) do
  if ent.DisplayName ~= nil then host = ent break end
end
print("picked: " .. tostring(host))'

grab entity-component-health '
local ents = Ext.Entity.GetAllEntitiesWithComponent("Health")
print("entities with Health: " .. #ents)
if ents[1] ~= nil then _D(ents[1].Health) end'

grab entity-component-list '
local ents = Ext.Entity.GetAllEntitiesWithComponent("Health")
local e = ents[1]
if e ~= nil then
  local names = {}
  for k, v in pairs(e:GetAllComponents()) do names[#names+1] = k end
  table.sort(names)
  print("components on one entity: " .. #names)
  print(table.concat(names, "\n"))
end'

echo "== the type system, which drives every shape =="
grab types-health '_D(Ext.Types.GetTypeInfo("eoc::HealthComponent"))'
grab types-count '
local t = Ext.Types.GetAllTypes()
print("types: " .. #t)
for i = 1, 25 do print(t[i]) end'

echo "== modules bg3le declares but has not filled in =="
grab mod-loadorder '
local order = Ext.Mod.GetLoadOrder()
print("mods: " .. #order)
for i = 1, math.min(#order, 8) do
  local m = Ext.Mod.GetMod(order[i])
  print(order[i] .. "  " .. tostring(m and m.Info and m.Info.Name))
end'
grab mod-shape '
local order = Ext.Mod.GetLoadOrder()
_D(Ext.Mod.GetMod(order[1]))'
grab utils-shape '
_D(Ext.Utils.GetGameState())
_D(Ext.Utils.Version())
print(Ext.Utils.GameVersion())'
grab loca-sample '
local k = Ext.Loca.GetTranslatedString("h5fafec24g30d5g425cg952cga9c53752059c")
print(tostring(k))'
grab vars-shape '
_D(Ext.Vars.GetModVariables and "GetModVariables present" or "absent")'

echo "== enums, which our metadata re-expands independently =="
grab enums-damagetype '
for _, v in ipairs(Ext.Enums.DamageType) do print(tostring(v)) end'
grab enums-list '
local names = {}
for k, _ in pairs(Ext.Enums) do names[#names+1] = k end
table.sort(names)
print("enums: " .. #names)
print(table.concat(names, "\n"))'
