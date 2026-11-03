#include "log.h"
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace fo {
static std::mutex g_mu;
static FILE* g_file = nullptr;

static std::wstring exe_dir() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);  // path of Fable.exe, not of this DLL
    std::wstring p(path);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

void log_open() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) return;
    std::wstring path = exe_dir() + L"\\fable_online.log";
    g_file = _wfopen(path.c_str(), L"a");
}

void log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::fprintf(g_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_file, fmt, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}

void log_close() {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}
}  // namespace fo
