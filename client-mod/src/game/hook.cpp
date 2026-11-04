#include "hook.h"

#include <windows.h>

#include <atomic>
#include <exception>
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
std::vector<std::function<void()>> g_frames;
std::atomic<DWORD> g_gameThread{0};
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
    for (auto& fn : due) {
        // A C++ exception must never unwind into Fable's message loop: that ends the game.
        try {
            fn();
        } catch (const std::exception& e) {
            fo::log("hook: a game-thread step failed (%s); carrying on", e.what());
        } catch (...) {
            fo::log("hook: a game-thread step failed; carrying on");
        }
    }
}

LRESULT CALLBACK wndproc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TIMER && wp == kTimerId) {
        g_gameThread.store(GetCurrentThreadId());
        run_ticks();
        return 0;
    }
    if (msg == WM_KEYDOWN && !(lp & (1 << 30))) {  // first press, not auto-repeat
        std::vector<std::function<void(int)>> keys;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            keys = g_keys;
        }
        for (auto& fn : keys) {
            try {
                fn(static_cast<int>(wp));
            } catch (...) {
                fo::log("hook: a key handler failed; carrying on");
            }
        }
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
    g_frames.clear();
}

void on_tick(std::function<void()> fn, unsigned everyMs) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_ticks.push_back(Tick{std::move(fn), everyMs < kTimerMs ? kTimerMs : everyMs, 0});
}

void on_frame(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_frames.push_back(std::move(fn));
}

void run_frame() {
    // Only where the timer steps run: the frame hooks write engine memory the game loop owns.
    DWORD game = g_gameThread.load();
    if (!game || GetCurrentThreadId() != game) return;
    std::vector<std::function<void()>> frames;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        frames = g_frames;
    }
    for (auto& fn : frames) {
        try {
            fn();
        } catch (...) {
            fo::log("hook: a frame step failed; carrying on");
        }
    }
}

void on_key(std::function<void(int)> fn) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_keys.push_back(std::move(fn));
}

}  // namespace fo::game::hook
