// Fable Online client mod, phase 1: prove we run inside Fable.exe.
//
// Loaded by the launcher (LoadLibrary in a suspended Fable.exe). Does no game
// hooking yet: it logs where it landed, which mode it runs in, and a heartbeat.
// Phase 2 adds hooks once tools/re-notes/ has the addresses.
// Chat and name tags: overlay/ (drawing and typing), net/ (chat server), ui/ (logic).
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "devkit.h"
#include "herolook.h"
#include <string>
#include <thread>
#include "config.h"
#include "game/hero.h"
#include "crash.h"
#include "log.h"
#include "net/chat_client.h"
#include "display/hud_scale.h"
#include "display/mouse_look.h"
#include "display/present.h"
#include "display/widescreen.h"
#include "menu/menu.h"
#include "overlay/overlay.h"
#include "probe.h"
#include "ui/json.h"
#include "sync.h"
#include "scoreboard.h"

namespace {
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr;
HMODULE g_self = nullptr;
bool g_online = false;   // client mode: chat and name tags on

enum class Mode { Client, Server };

// FABLE_ONLINE_MODE=server makes this copy a region authority (phase 5+).
Mode read_mode() {
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("FABLE_ONLINE_MODE", buf, sizeof(buf));
    return (n > 0 && _stricmp(buf, "server") == 0) ? Mode::Server : Mode::Client;
}

// Chat connection, hook report, and hero position for name tags (client mode only).
void online_thread() {
    const fo::Config& cfg = fo::config();
    std::thread(fo::overlay::install_late).detach();
    // Realm chat only takes the token the game server signs in /join, which the launcher
    // passes as FABLE_ONLINE_TOKEN. Without one there is nothing to log in with.
    if (cfg.chat_enabled && cfg.token.empty()) {
        fo::log("chat: no login token (start Fable from the launcher); Realm chat is off");
    } else if (cfg.chat_enabled) {
        std::string hello = "{\"t\":\"hello\",\"v\":1,\"token\":" + fo::json::quote(cfg.token) + "}";
        fo::log("chat: server %s:%u, token login", cfg.chat_host.c_str(), cfg.chat_port);
        fo::net::chat_start(cfg.chat_host, cfg.chat_port, hello);
    }
    fo::game::HeroPos last;
    unsigned ticks = 0;
    bool was_connected = false;
    while (g_running.load()) {
        Sleep(200);
        fo::game::HeroPos p;
        try {
            p = fo::game::hero_read();
        } catch (...) {
            fo::log("hero: read failed; trying again");
        }
        fo::overlay::set_hero(p);
        // Interim (dev servers only): report our position so others see our name tag.
        // Production servers ignore this; the region server owns positions.
        bool connected = fo::net::chat_connected();
        bool moved = p.valid && (!last.valid || p.area != last.area || std::fabs(p.x - last.x) + std::fabs(p.y - last.y) + std::fabs(p.z - last.z) > 0.05f);
        if (connected && p.valid && (moved || !was_connected || ticks % 25 == 0)) {
            fo::net::chat_send("{\"t\":\"pos\",\"region\":" + std::to_string(p.area) + ",\"x\":" + fo::json::number(p.x) +
                               ",\"y\":" + fo::json::number(p.y) + ",\"z\":" + fo::json::number(p.z) + "}");
            last = p;
        }
        was_connected = connected;
        ++ticks;
        if (ticks == 50 || ticks == 150 || ticks % 1500 == 0) { fo::overlay::log_stats(); fo::menu::log_status(); if (ticks < 200) fo::display::log_status(); }
        if (ticks % 50 == 0 && (ticks <= 1500 || ticks % 1500 == 0)) fo::display::log_frame_rate();  // 10 s, 30 s, then every 5 min
    }
    fo::net::chat_stop();
}

DWORD WINAPI main_thread(LPVOID) {
    fo::log_open();
    fo::crash::install();
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
    fo::scoreboard_report();

    char probe[4] = {};
    bool probing = GetEnvironmentVariableA("FABLE_ONLINE_PROBE", probe, sizeof(probe)) > 0 && probe[0] == '1';
    fo::log("  probe: %s (build 6: devkit, herolook)", probing ? "on" : "off");

    char dev[4] = {};
    if (GetEnvironmentVariableA("FABLE_ONLINE_DEV", dev, sizeof(dev)) > 0 && dev[0] == '1') fo::devkit::install();

    std::thread online;
    if (g_online) online = std::thread(online_thread);
    // Entity sync: other players and the server's creatures. Off while probing, which
    // walks the whole address space and must not have puppets appearing underneath it.
    if (!probing) fo::sync::start();

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
    if (online.joinable()) online.join();
    fo::sync::stop();
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
        // Byte patches only (no locks, no allocation), so safe here and done before the game
        // thread is resumed and builds its scoreboard client.
        fo::scoreboard_patch();
        g_running = true;
        g_self = module;
        // The game is still suspended here, so patch its Direct3D and DirectInput imports
        // now, before it creates its device. Import-table writes only: safe under the loader lock.
        fo::config_load(module);
        fo::menu::set_module(module);
        g_online = read_mode() == Mode::Client;
        if (g_online) fo::overlay::install_early();
        if (g_online) fo::display::install();   // a few code bytes and a VirtualAlloc: fine here
        if (g_online) fo::display::hud::install();
        if (g_online) fo::display::mouse_look::install();
        // Never do real work inside DllMain (loader lock): hand off to a thread.
        g_thread = CreateThread(nullptr, 0, main_thread, nullptr, 0, nullptr);
        break;
    case DLL_PROCESS_DETACH:
        g_running = false;
        break;
    }
    return TRUE;
}
