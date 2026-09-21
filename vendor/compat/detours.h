#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// Microsoft Detours is Windows-only. The only place upstream uses it is
// CoreLib/Wrappers.h, which inline-hooks an arbitrary function pointer -- and
// the only users of that are the Osiris wrappers, which bg3le replaces with
// PLT interposition. Rather than port a general inline hooker (which would
// need an instruction-length decoder we have so far avoided), these report
// failure, and Wrap() already handles that by clearing itself and logging.
//
// If something genuinely needs inline hooking later, implement these over
// bg3le's own primitives rather than widening this shim.
//

typedef void* PVOID;
typedef void* PDETOUR_TRAMPOLINE;

#ifndef NO_ERROR
#define NO_ERROR 0
#endif
#define ERROR_NOT_SUPPORTED 50

extern "C" {

long DetourAttachEx(PVOID* ppPointer, PVOID pDetour,
                    PDETOUR_TRAMPOLINE* ppRealTrampoline, PVOID* ppRealTarget,
                    PVOID* ppRealDetour);
long DetourDetach(PVOID* ppPointer, PVOID pDetour);
}
