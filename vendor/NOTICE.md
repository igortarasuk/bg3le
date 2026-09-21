# Third-party code

## bg3se — Baldur's Gate 3 Script Extender

`vendor/bg3se/` is a partial copy of the [Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se) by **Norbyte** and the bg3se
contributors, licensed MIT with the Commons Clause (see `vendor/bg3se/LICENSE`).

**Thank you.** The 640 component definitions, the Lua binding framework and the
code generators in this directory represent an enormous amount of reverse
engineering. bg3le reuses them rather than rediscovering them, and would not be
a realistic project otherwise.

Copied subsystems: `CoreLib/`, `BG3Extender/GameDefinitions/`,
`BG3Extender/Lua/`, plus the handful of files from `BG3Extender/Extender/` and
`BG3Extender/GameHooks/` those depend on.

### Changes made

The upstream build is MSVC-only. Every change below exists solely to compile
the same code with clang, and each is a construct MSVC would also accept, so
they are candidates for an upstream conformance PR. Nothing was changed for
behaviour.

**A requires-clause cannot begin with `!` on a primary expression** — wrapped
the constraint in parentheses:

- `BG3Extender/GameDefinitions/Base/BaseTypeInformation.h:349`
- `BG3Extender/Lua/Helpers/LuaGet.h:295,298`
- `BG3Extender/Lua/Helpers/LuaGetObject.h:5,13`
- `BG3Extender/Lua/Helpers/LuaTypeCheck.h:241,244`
- `BG3Extender/Lua/Helpers/LuaTypeCheckObject.h:26,38`
- `BG3Extender/Lua/LuaSerializers.h:543`

**Stray `typename` before a builtin type** — removed:

- `BG3Extender/Lua/Helpers/LuaTypeCheckObject.h:21`
- `BG3Extender/Lua/Shared/LuaTypeValidators.h:302,723,731`

**Friend declaration of a class template without its arguments** —
`friend FunctionImpl;` became
`template <class TFun, class TData> friend class FunctionImpl;`:

- `CoreLib/Base/BaseFunction.h:186`

**`__FUNCTION__` is a variable under clang, not a string literal**, so it
cannot be concatenated at compile time. The stringstream macros now stream it
and the `...S` macros build the string at runtime:

- `BG3Extender/Extender/Shared/Utils.h` (`LuaError`, `OsiError`, `OsiWarn`,
  `OsiErrorS`, `OsiWarnS`, `OsiMsgS`)

**UTF-16 sources** — MSVC accepts them, clang does not. Transcoded to UTF-8:

- `CoreLib/Config.h`
- `BG3Extender/Extender/BuildInfo.h`

### Generated files

Upstream gitignores these; they are committed here so the tree builds without
a generation step. Regenerate with the upstream scripts, which run unchanged
under python3:

    python3 vendor/bg3se/BG3Extender/make_enumerations.py
    python3 vendor/bg3se/BG3Extender/make_property_map.py
    protoc --cpp_out=vendor/bg3se/BG3Extender Extender/Shared/ExtenderProtocol.proto

## Not vendored

- **NoesisGUI** — proprietary SDK fetched by `tools/fetch-externals.sh`, which
  also drops a stray `override` in `NsCore/TypePropertyImpl.h` that clang
  rejects (the base declares `GetCopy`, not that overload, so it never
  overrode anything).
- **oneTBB** (Apache-2.0) — `vendor/compat/concurrent_{vector,queue}.h` map
  MSVC's `concurrency::` containers onto it.
- **glm, imgui, lua, rapidjson, tinycrypt, Vulkan-Headers** — fetched, each
  under its own license.

`vendor/compat/` is bg3le's own code.
