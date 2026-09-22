-- How much of bg3se's public API bg3le implements, measured against the
-- surface captured from the real extender in reference/ext-api-surface.txt.
--
-- Run it through the debugger:
--   client/bg3lua -e "$(cat tools/api-coverage.lua)"
--
-- The point is a number that has to reach zero, not a vibe: a mod written
-- against bg3se calls these by name, so a missing name is a mod that does
-- not run.
local function surface()
  local out = {}
  for name, value in pairs(Ext) do
    if type(value) == "function" then
      out["Ext." .. name] = true
    elseif type(value) == "table" then
      local any = false
      for k, v in pairs(value) do
        out["Ext." .. name .. "." .. k] = true
        any = true
      end
      if not any then out["Ext." .. name] = true end
    end
  end
  return out
end

local have = surface()
local missing, extra, total = {}, {}, 0
local byModule = {}

for line in io.lines("reference/ext-api-surface.txt") do
  local module, body = line:match("^(Ext%.%w+) = {(.*)}$")
  if module then
    local short = module:gsub("^Ext%.", "")
    byModule[short] = byModule[short] or {have = 0, want = 0}
    for fn in body:gmatch("(%w+)%(function%)") do
      total = total + 1
      byModule[short].want = byModule[short].want + 1
      if have[module .. "." .. fn] then
        byModule[short].have = byModule[short].have + 1
      else
        missing[#missing + 1] = module .. "." .. fn
      end
    end
  else
    local fn = line:match("^(Ext%.%w+) : function$")
    if fn then
      total = total + 1
      byModule["<top level>"] = byModule["<top level>"] or {have = 0, want = 0}
      byModule["<top level>"].want = byModule["<top level>"].want + 1
      if have[fn] then
        byModule["<top level>"].have = byModule["<top level>"].have + 1
      else
        missing[#missing + 1] = fn
      end
    end
  end
end

local names = {}
for k in pairs(byModule) do names[#names + 1] = k end
table.sort(names)

print(string.format("%-22s %5s %5s", "module", "have", "want"))
for _, n in ipairs(names) do
  local m = byModule[n]
  local mark = (m.have == m.want) and "  complete" or ""
  print(string.format("%-22s %5d %5d%s", n, m.have, m.want, mark))
end
print(string.format("\ntotal %d of %d (%.1f%%), %d missing",
  total - #missing, total, (total - #missing) / total * 100, #missing))

if os.getenv("BG3LE_LIST_MISSING") then
  table.sort(missing)
  for _, m in ipairs(missing) do print("  missing " .. m) end
end
