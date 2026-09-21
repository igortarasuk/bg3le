// bg3se::Console for Linux.
//
// The upstream implementation (vendor/bg3se/CoreLib/Console.cpp) drives a
// Win32 console: AllocConsole, screen buffers, font tables, colour
// attributes. None of that exists here, so this provides the same interface
// over stdout, ANSI colour, and bg3le's own log and debugger output.
//
// The interface is declared by CoreLib/Console.h, by Norbyte and the bg3se
// contributors (https://github.com/Norbyte/bg3se); this implementation is
// ours.

#include <CoreLib/Console.h>

#include <cstdio>
#include <string>

#include "../debug_server.h"
#include "../log.h"

BEGIN_SE()

namespace {

// Matched to DebugMessageType, so output is readable in a normal terminal.
const char* colour_for(DebugMessageType type)
{
    switch (type) {
    case DebugMessageType::Error:   return "\x1b[1;31m";
    case DebugMessageType::Warning: return "\x1b[1;33m";
    case DebugMessageType::Osiris:  return "\x1b[0;36m";
    case DebugMessageType::Debug:   return "\x1b[0;90m";
    default:                        return "";
    }
}

// bg3lua expects the severities the debugger protocol defines.
int severity_for(DebugMessageType type)
{
    switch (type) {
    case DebugMessageType::Error:   return 2;
    case DebugMessageType::Warning: return 1;
    default:                        return 0;
    }
}

}  // namespace

Console::~Console() = default;

void Console::Create()
{
    // There is no console to allocate: bg3le is loaded into a process that
    // already has a terminal, or the CreateConsole setting opened one.
    created_ = true;
    enabled_ = true;
}

void Console::Destroy()
{
    CloseLogFile();
    created_ = false;
}

void Console::OpenLogFile(std::wstring const& path)
{
    if (logToFile_) CloseLogFile();

    // libc++ has no wide-path fstream constructor.
    logFile_.open(ToUTF8(path).c_str(), std::ios::out | std::ios::app);
    logToFile_ = logFile_.good();
    if (!logToFile_) {
        bg3le::logf("console: could not open log file %s", ToUTF8(path).c_str());
    }
}

void Console::CloseLogFile()
{
    if (!logToFile_) return;
    logFile_.close();
    logToFile_ = false;
}

void Console::SetColor(DebugMessageType type)
{
    std::fputs(colour_for(type), stdout);
}

void Console::LocalPrint(DebugMessageType type, char const* msg)
{
    if (silence_ || msg == nullptr) return;

    SetColor(type);
    std::fputs(msg, stdout);
    std::fputs("\x1b[0m\n", stdout);
    std::fflush(stdout);

    if (logToFile_) {
        logFile_ << msg << std::endl;
    }
    if (logCallback_ != nullptr) {
        logCallback_(type, msg);
    }
}

void Console::Print(DebugMessageType type, char const* msg)
{
    LocalPrint(type, msg);
    // Unlike LocalPrint, this also reaches an attached bg3lua client.
    if (msg != nullptr) {
        bg3le::debug_server_output(msg, severity_for(type));
    }
}

void Console::Clear()
{
    std::fputs("\x1b[2J\x1b[H", stdout);
    std::fflush(stdout);
}

void Console::EnableOutput(bool enabled)
{
    silence_ = !enabled;
}

void Console::SetLogCallback(LogCallbackProc* callback)
{
    logCallback_ = callback;
}

END_SE()
