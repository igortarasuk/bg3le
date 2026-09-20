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

// Records the calling thread as the one safe for Lua and engine calls.
// Called from Osiris entry points, which run on the story thread.
void debug_server_note_story_thread();

// Cheap poll from a hot interposed libc call: pumps only when work is
// pending and we are on the story thread. Designed to cost ~nothing
// otherwise, since it runs on every clock_gettime.
void debug_server_tick();

}  // namespace bg3le
