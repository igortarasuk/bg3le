#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// Only PathFileExistsW is used.
//

#include <unistd.h>

#include <cstdlib>
#include <cwchar>
#include <string>

inline int PathFileExistsW(const wchar_t* path) {
    if (path == nullptr) return 0;
    const std::size_t needed = std::wcstombs(nullptr, path, 0);
    if (needed == static_cast<std::size_t>(-1)) return 0;
    std::string narrow(needed + 1, '\0');
    std::wcstombs(narrow.data(), path, narrow.size());
    narrow.resize(needed);
    return ::access(narrow.c_str(), F_OK) == 0 ? 1 : 0;
}
