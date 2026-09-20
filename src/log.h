#pragma once
namespace bg3le {
void log_init();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Path of this process's log file, or "" before log_init().
const char* log_path();
}
