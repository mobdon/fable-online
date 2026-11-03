#include "hook.h"

#include <windows.h>

#include <mutex>
#include <vector>

#include "../log.h"

namespace fo::game::hook {
namespace {

constexpr UINT_PTR kTimerId = 0xF0;
constexpr unsigned kTimerMs = 16;  // the finest tick we offer; callers ask for multiples

struct Tick {
    std::function<void()> fn;
    unsigned everyMs;
    DWORD lastMs;
};

std::mutex g_mu;
std::vector<Tick> g_ticks;
std::vector<std::function<void(int)>> g_keys;
HWND g_window = nullptr;
WNDPROC g_prev = nullptr;

void run_ticks() {
    DWORD now = GetTickCount();
    std::vector<std::function<void()>> due;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        for (Tick& t : g_ticks)
            if (now - t.lastMs >= t.everyMs) {
                t.lastMs = now;
                due.push_back(t.fn);
            }
    }
    for (auto& fn : due) fn();
}

LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kTimerId) {
        run_ticks();
        return 0;
    }
    if (msg == WM_KEYDOWN && !(lp & (1 << 30))) {  // first press, not auto-repeat
        std::vector<std::function<void(int)>> keys;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            keys = g_keys;
        }
        for (auto& fn : keys) fn(static_cast<int>(wp));
    }
    return CallWindowProcW(g_prev, w, msg, wp, lp);
}

BOOL CALLBACK find_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

}  // namespace

bool attached() { return g_window != nullptr; }

bool install() {
    if (g_window) return true;
    HWND w = nullptr;
    for (int i = 0; i < 120 && !w; ++i) {
        EnumWindows(find_window, reinterpret_cast<LPARAM>(&w));
        if (!w) Sleep(1000);
    }
    if (!w) {
        fo::log("hook: game window not found; nothing that needs the game thread will run");
        return false;
    }
    g_prev = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(wndproc)));
    g_window = w;
    SetTimer(w, kTimerId, kTimerMs, nullptr);
    fo::log("hook: attached to the game window");
    return true;
}

void remove() {
    if (!g_window) return;
    KillTimer(g_window, kTimerId);
    if (g_prev) SetWindowLongPtrW(g_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_prev));
    g_window = nullptr;
    g_prev = nullptr;
    std::lock_guard<std::mutex> lock(g_mu);
    g_ticks.clear();
    g_keys.clear();
}

void on_tick(std::function<void()> fn, unsigned everyMs) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ticks.push_back(Tick{std::move(fn), everyMs < kTimerMs ? kTimerMs : everyMs, 0});
}

void on_key(std::function<void(int)> fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_keys.push_back(std::move(fn));
}

}  // namespace fo::game::hook
