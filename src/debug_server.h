// LuaDebug-protocol server, so existing clients (bg3lua) work unchanged.
#pragma once

namespace bg3le {

// Starts the listener thread. No-op if already running or BG3LE_DEBUG=0.
void debug_server_start();

// Runs queued evaluations. Must be called from the story thread; the socket
// thread never touches Lua or the engine itself.
void debug_server_pump();

// Forwards Lua print() output to the attached client.
void debug_server_output(const char* text);

}  // namespace bg3le
