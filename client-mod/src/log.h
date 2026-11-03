#pragma once
// Tiny thread-safe logger. Writes fable_online.log next to Fable.exe.
namespace fo {
void log_open();
void log(const char* fmt, ...);
void log_close();
}
