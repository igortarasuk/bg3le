// CreateConsole parity: open a terminal running the bg3lua client at
// startup, the way the Windows extender opens its console window.
#pragma once

namespace bg3le {

// Opens the console if enabled. Enabled by "CreateConsole": true in
// ScriptExtenderSettings.json next to the game binary, or BG3LE_CONSOLE=1.
// Safe to call more than once; only the first call spawns.
void maybe_open_console();

}  // namespace bg3le
