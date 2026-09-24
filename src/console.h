// CreateConsole parity: open a terminal running the bg3lua client at
// startup, the way the Windows extender opens its console window.
#pragma once

#include <string>

namespace bg3le {

// Opens the console if enabled. Enabled by "CreateConsole": true in
// ScriptExtenderSettings.json next to the game binary, or BG3LE_CONSOLE=1.
// Safe to call more than once; only the first call spawns.
void maybe_open_console();

// Reads a boolean key from ScriptExtenderSettings.json next to the
// binary. Missing file or key yields the fallback.
bool settings_flag(const std::string& exe_dir, const char* key, bool fallback);

}  // namespace bg3le
