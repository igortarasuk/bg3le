# Third-party code

## bg3se — Baldur's Gate 3 Script Extender

`vendor/bg3se/` is a partial copy of the [Baldur's Gate 3 Script
Extender](https://github.com/Norbyte/bg3se) by **Norbyte** and the bg3se
contributors, licensed MIT with the Commons Clause (see `vendor/bg3se/LICENSE`).

**Thank you.** The 640 component definitions, the Lua binding framework, the
extender core and the code generators in this directory represent an enormous
amount of reverse engineering. bg3le reuses them rather than rediscovering
them, and would not be a realistic project otherwise.

Copied subsystems: `CoreLib/`, `BG3Extender/GameDefinitions/`,
`BG3Extender/Lua/`, `BG3Extender/Extender/`, `BG3Extender/GameHooks/`,
`BG3Extender/Osiris/`, plus `stdafx.h`, `resource.h` and
`BG3Updater/ExtenderAPI.h`.

`external/lua` is Norbyte's Lua fork; see
[external/lua/README.bg3le](../external/lua/README.bg3le) for why that one is
not optional.

### Changes made

The upstream build is MSVC-only. Every change below exists solely to compile
the same code with clang, and each is a construct MSVC would also accept, so
they are candidates for an upstream conformance PR. Nothing was changed for
behaviour.

Run `tools/check-vendor-patches.py` to confirm they are all still applied —
re-copying files from an upstream checkout silently reverts them.

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

**A variadic macro leaves a trailing comma when given no variadic argument.**
MSVC drops it; standard C++ needs `__VA_OPT__`. Applied to `DEBUG`, `INFO`,
`WARN`, `ERR`, their `_LOCAL` variants and `WARN_ONCE`:

- `CoreLib/Utils.h:10-21`

**`__FUNCTION__` is a variable under clang, not a string literal**, so it
cannot be concatenated at compile time. The stringstream macros stream it and
the `...S` macros build the string at runtime:

- `BG3Extender/Extender/Shared/Utils.h` (`LuaError`, `OsiError`, `OsiWarn`,
  `OsiErrorS`, `OsiWarnS`, `OsiMsgS`)

**`std::thread` was forward-declared.** libc++ declares it in an inline
namespace, so a second declaration is a distinct type and every use becomes
ambiguous. Replaced with `#include <thread>`:

- `BG3Extender/Extender/Shared/Utils.h:6`

**`FixedStringUnhashed` had no stream operator.** It is a sibling of
`FixedString`, not a `FixedString`, so the existing overload did not apply and
insertion was ambiguous between the base class conversions to `char const*`
and to `StringView`. Added the matching overload:

- `CoreLib/Base/BaseString.h`

**A static data member of a class template specialisation needs `template<>`**:

- `BG3Extender/Extender/Client/SDLManager.h:15` (the `SDL_HOOK` macro)
- `BG3Extender/Lua/Libs/ClientUI/Symbols.inl:11` (the `FOR_NOESIS_TYPE` macro,
  expanded for 23 Noesis types)

**`operator new` must take `size_t` exactly.** Upstream declares it as
`unsigned __int64`, which is the same width as LP64 `size_t` but a different
type (`unsigned long long` vs `unsigned long`):

- `BG3Extender/Lua/Libs/ClientUI/Builtins.inl:169,174`

**A `void*` cannot be `static_cast` to a function pointer**; that needs
`reinterpret_cast`:

- `BG3Extender/Lua/Libs/ClientUI/NsHelpers.inl:717`

**SFINAE has to depend on a parameter of the function template, not of the
enclosing class.** `TryOpOrFail<T>` detects whether `T` has `Do`/`DoInPlace`
via a trailing return type, but `T` is fixed once the class is instantiated,
so a missing member is a hard error rather than a substitution failure in the
immediate context. Aliasing `T` as a defaulted parameter on each member
template moves the lookup to where SFINAE applies:

- `BG3Extender/Lua/Libs/Math.inl` (`TryOpOrFail::Do` and `::DoInPlace`)

**`lua_Integer` is `long long`, while `int64_t` is `long` on LP64**, so
constructing `std::variant<char const*, int64_t, double>` from a `lua_Integer`
has no viable alternative without an explicit cast. On MSVC they are the same
type:

- `BG3Extender/Lua/Libs/Json.inl:373,375`

**An `STDString` was passed through a variadic function.** That is undefined on
both platforms; MSVC only warns:

- `BG3Extender/Lua/Libs/ClientAudio.inl:56` — now passes `c_str()`

**`std::derived_from` requires complete types**, so testing an incomplete type
is a hard error rather than a false. `IsArray` is evaluated against types that
are only forward-declared at that point, so the check is now guarded by an
`IsCompleteType` concept:

- `BG3Extender/GameDefinitions/Base/TypeMetadata.h:19`

**Declaring any class-scope `operator delete` hides the global ones**, and an
inherited virtual destructor still needs a usual deallocation function.
`NsCustomDataContext` declared only the placement form:

- `BG3Extender/Lua/Libs/ClientUI/CustomProperties.inl`

**A non-type template parameter must match the exact type to deduce.**
`std::array`'s extent is `std::size_t`, so `template <class T, int Size>` never
matches it. MSVC deduces anyway. This one accounted for 71 errors in
`LuaObjectProxies.cpp` alone, and their own serialisation helpers already used
`size_t`:

- `BG3Extender/Lua/Shared/Proxies/LuaArrayProxy.h:641,647`
- `BG3Extender/GameDefinitions/Base/BaseTypeInformation.h:270`

**Converting a function pointer to `void*` is conditionally supported**, not
standard. MSVC does it implicitly; the casts are now explicit:

- `BG3Extender/Lua/Shared/Proxies/LuaObjectProxies.cpp` (the `P_FALLBACK` macro)

**libc++ has no wide-path `fstream` constructor**; MSVC provides one as an
extension. These now convert with upstream's own `ToUTF8`:

- `BG3Extender/Lua/Shared/LuaBundle.cpp:39`
- `CoreLib/Crypto.cpp:85`

**An include used the wrong directory case**, which resolves on Windows and
not on Linux:

- `BG3Extender/Lua/Shared/LuaStats.h:4` — `lua/LuaBinding.h` → `Lua/LuaBinding.h`

**UTF-16 sources** — MSVC accepts them, clang does not. Transcoded to UTF-8:

- `CoreLib/Config.h`
- `BG3Extender/Extender/BuildInfo.h`

### Generated files

Upstream gitignores these; they are committed here so the tree builds without
a generation step. Regenerate with the upstream scripts, which run unchanged
under python3, and with protoc:

    python3 vendor/bg3se/BG3Extender/make_enumerations.py
    python3 vendor/bg3se/BG3Extender/make_property_map.py
    cd vendor/bg3se/BG3Extender
    protoc --cpp_out=. Extender/Shared/ExtenderProtocol.proto
    protoc --cpp_out=. Osiris/Debugger/osidebug.proto
    protoc --cpp_out=. Lua/Debugger/LuaDebug.proto

## vendor/compat — bg3le's own code

Shims that let the upstream sources compile unmodified. They are force-included
or sit ahead of the vendored tree on the include path.

- `msvc_compat.h` — SAL annotations, Win32 typedefs (`DWORD` and `LONG` are
  32-bit on Windows, so they are `unsigned int` and `int`, not `long`), the
  MSVC bit-scan intrinsics, the secure-CRT `sprintf_s` family, `_strdup`,
  `VirtualProtect` over `mprotect`, `QueryPerformanceCounter` over
  `CLOCK_MONOTONIC`, `GetCommandLineW` over `/proc/self/cmdline`,
  `GetProcAddress`/`GetModuleHandleW` over `dlsym`/`dlopen`, critical sections
  over recursive `pthread_mutex`, and the byte-swap and Interlocked intrinsics
  over the compiler builtins. Basic Win32 typedefs are declared first, since
  the rest of the header uses them
- `Shlwapi.h`, `shlwapi.h`, `combaseapi.h`, `WS2tcpip.h` — `PathFileExistsW`,
  the RPC UUID functions (faithful to the Windows GUID layout, since Guid
  values round-trip through Osiris), and the TCP/IP half of Winsock
- `concurrent_vector.h`, `concurrent_queue.h`, `ppl.h` — MSVC's
  `concurrency::` containers mapped onto [oneTBB](https://github.com/uxlfoundation/oneTBB)
  (Apache-2.0)
- `WinSock2.h` — the Osiris debugger interface is written against Winsock;
  Berkeley sockets map directly
- `detours.h` — declarations only. The sole upstream user is
  `CoreLib/Wrappers.h`, whose callers bg3le replaces with PLT interposition, so
  these refuse rather than hook; `Wrap()` already handles a non-zero return

## Not vendored

- **NoesisGUI** — proprietary SDK fetched by `tools/fetch-externals.sh`, which
  also drops a stray `override` in `NsCore/TypePropertyImpl.h` that clang
  rejects (the base declares `GetCopy`, not that overload, so it never
  overrode anything).
- **glm, imgui, rapidjson, tinycrypt, optick, Vulkan-Headers** — fetched, each
  under its own license.
- **protobuf, SDL2, oneTBB** — from the distribution.
