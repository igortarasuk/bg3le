#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// A few upstream headers include <windows.h> for a handful of typedefs. The
// types themselves live in msvc_compat.h, which is force-included, so this
// only needs to exist to satisfy the include.
//
