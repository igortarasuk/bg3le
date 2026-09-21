#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// MSVC ships concurrency::concurrent_vector in <concurrent_vector.h>.
// oneTBB provides the same container as tbb::concurrent_vector.
#include <tbb/concurrent_vector.h>
namespace concurrency { using tbb::concurrent_vector; }
