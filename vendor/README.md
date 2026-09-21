# Compiling bg3se headers on Linux

## Credits

Everything this directory builds against is the work of **Norbyte** and the
contributors to the [Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se), licensed MIT with the Commons
Clause. The component definitions, the Lua binding framework and the code
generators are theirs; this directory only carries the glue needed to compile
them with clang instead of MSVC. Thank you — reusing this work saves an
enormous amount of effort, and any file here that ports or adapts it says so
at the top.

`concurrent_vector`/`concurrent_queue` are shimmed onto
[oneTBB](https://github.com/uxlfoundation/oneTBB) (Apache-2.0).

## Purpose

bg3le is its own project; bg3se is reference material. This directory records
what it takes to compile bg3se's headers with clang, because that was
non-obvious and expensive to work out, and because reusing its component
definitions and Lua binding layer is the cheapest route to `Ext.Entity` and to
the 275-function `Ext.*` surface.

bg3se is vendored under `vendor/bg3se/` with the clang fixes applied; see
`vendor/NOTICE.md` for attribution and the full list of changes.

## Bootstrap

    tools/fetch-externals.sh    # Noesis, glm, imgui, lua, rapidjson, Vulkan
    tools/check-vendor.sh       # compiles the vendored headers

## Result

All 640 `DEFINE_COMPONENT` definitions compile unmodified. Components are
declared as plain structs keyed by the name the engine registers them under:

    struct HealthComponent : public BaseComponent {
        DEFINE_COMPONENT(Health, "eoc::HealthComponent")
        int Hp; int MaxHp; ...
    };

No offsets are hardcoded, so the compiler derives the layout, and the
container types are almost all hand-rolled and therefore ABI-neutral. That
string is what the engine's runtime registry is keyed on, and it appears
verbatim in our recovered symbols.

## libc++ is required, not optional

The native game is built against libc++ (the `std::__2` inline namespace), so
`STDString` is 24 bytes there and here. libstdc++ would give 32 and silently
shift every field after a string in a component.

## Flags that matter

- `-fdeclspec -fms-extensions` — `__declspec` and MSVC struct extensions
- `-fdelayed-template-parsing` — MSVC resolves dependent names at instantiation
- `-DGLM_FORCE_INTRINSICS` — MSVC enables SIMD implicitly; without this glm
  disables aligned gentypes and its own headers fail to compile
- **not** `-fms-compatibility` — it de-keywords `char16_t`/`char32_t` and
  breaks libc++

## What clang rejects

MSVC accepts all of these. Each is a one-line fix that MSVC would *still*
accept, so they belong in a patch series (and eventually a small upstream
conformance PR) rather than in `compat/`, which cannot shim syntax.

Compiling `GameDefinitions/Components/Components.h` needs three:

    CoreLib/Base/BaseFunction.h:186
        friend declaration of a class template without its arguments
    BG3Extender/GameDefinitions/Base/BaseTypeInformation.h:349
        requires-clause needs parentheses
    BG3Extender/Lua/Shared/Proxies/TypePropertyImpl.h:105
        override on a non-virtual member

Compiling the Lua binding layer (`Lua/LuaBinding.h`) adds one recurring
pattern and one one-off:

    requires-clause needs parentheses, at 8+ further sites including
        Lua/Shared/Proxies/LuaGet.h:295,298
        Lua/Shared/Proxies/LuaGetObject.h:5,13
        Lua/Shared/Proxies/LuaTypeCheck.h:241,244
        Lua/Shared/Proxies/LuaTypeCheckObject.h:26,38
    Lua/Shared/Proxies/LuaTypeCheckObject.h:21
        qualified name required after typename

## Code generation

Both generators run unchanged under python3 on Linux and must be run before
any compile, since `GameDefinitions/Generated/` is gitignored upstream:

    BG3Extender/make_enumerations.py     # Enumerations.inl, EnumerationMeta.h
    BG3Extender/make_property_map.py     # PropertyMaps.inl, ComponentTypes.inl

## Hazards

- `SRWLOCK` is shimmed as one pointer so declarations parse. The Linux engine
  uses pthread primitives, so any structure whose layout must match the engine
  and which embeds one is wrong until checked.
- `DWORD` and `LONG` are 32-bit on Windows, so they are typedefs of
  `unsigned int` and `int` — not of `long`, which is 64-bit on LP64.
- Detours is deliberately absent; bg3le hooks via PLT interposition,
  vtable-slot patching and call-site patching.
