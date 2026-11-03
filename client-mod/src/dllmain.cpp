// Fable Online client mod, phase 1: prove we run inside Fable.exe.
//
// Loaded by the launcher (LoadLibrary in a suspended Fable.exe). Does no game
// hooking yet: it logs where it landed, which mode it runs in, and a heartbeat.
// Phase 2 adds hooks once tools/re-notes/ has the addresses.
#include <windows.h>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include "devkit.h"
#include "herolook.h"
#include "log.h"
#include "probe.h"

namespace {
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;

enum class Mode { Client, Server };

// FABLE_ONLINE_MODE=server makes this copy a region authority (phase 5+).
Mode read_mode() {
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("FABLE_ONLINE_MODE", buf, sizeof(buf));
    return (n > 0 && _stricmp(buf, "server") == 0) ? Mode::Server : Mode::Client;
}

DWORD WINAPI main_thread(LPVOID) {
    fo::log_open();
    HMODULE exe = GetModuleHandleW(nullptr);
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    char narrow[MAX_PATH * 2] = {};
    WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);

    Mode mode = read_mode();
    fo::log("Fable Online mod loaded into Fable.exe (pid %lu)", GetCurrentProcessId());
    fo::log("  exe: %s", narrow);
    fo::log("  image base: 0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(exe)));
    fo::log("  mode: %s", mode == Mode::Server ? "server" : "client");

    char probe[4] = {};
    bool probing = GetEnvironmentVariableA("FABLE_ONLINE_PROBE", probe, sizeof(probe)) > 0 && probe[0] == '1';
    fo::log("  probe: %s (build 6: devkit, herolook)", probing ? "on" : "off");

    char dev[4] = {};
    if (GetEnvironmentVariableA("FABLE_ONLINE_DEV", dev, sizeof(dev)) > 0 && dev[0] == '1') fo::devkit::install();

    unsigned beat = 0;
    while (g_running.load()) {
        if (probing) {
            Sleep(2000);
            fo::probe_tick();
            if (++beat % 15 == 0) fo::log("heartbeat %u", beat / 15);
        } else {
            Sleep(5000);
            fo::log("heartbeat %u", ++beat);
        }
    }
    fo::log("mod shutting down");
    fo::log_close();
    return 0;
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(module);
        // Before the game is resumed, so the hooks see graphics.big open (test-only, env-gated).
        fo::herolook::install();
        g_running = true;
        // Never do real work inside DllMain (loader lock): hand off to a thread.
        g_thread = CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        break;
    }
    return TRUE;
}
