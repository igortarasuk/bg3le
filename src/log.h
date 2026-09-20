#pragma once
namespace bg3le {
void log_init();
void logf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
}
