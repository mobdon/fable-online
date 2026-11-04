#include "present.h"
#include <atomic>
#include <cstdio>
#include "../config.h"
#include "../log.h"
#include "hud_scale.h"
#include "mouse_look.h"

namespace fo::display {
namespace {
// One event query per frame in flight: Present waits for the frame before last, so the
// driver can queue one finished frame (keeping the CPU and GPU working side by side) but
// never the usual three, which is what makes the camera trail the mouse.
constexpr int kFramesAhead = 2;
IDirect3DQuery9* g_query[kFramesAhead] = {};
bool g_query_failed = false;
bool g_issued[kFramesAhead] = {};
unsigned g_slot = 0;
// Frame timing (render thread writes, log thread reads; plain atomics are enough).
std::atomic<unsigned> g_frames{0};
std::atomic<double> g_worst_ms{0};
LARGE_INTEGER g_freq = {}, g_last = {};

UINT g_game_w = 0, g_game_h = 0;   // back-buffer size the game asked for

// Puts the window in the [display] window style, sized for a w x h picture.
void style_window(HWND w, UINT bw, UINT bh) {
    const WindowMode mode = config().window_mode;
    if (!w || mode == WindowMode::Fullscreen) return;
    HMONITOR mon = MonitorFromWindow(w, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoW(mon, &mi);
    if (mode == WindowMode::Borderless) {
        // Cover the whole monitor; Direct3D scales the picture to fit, so pick your
        // monitor's resolution in Fable's Options for a sharp image.
        SetWindowLongPtrW(w, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
        const RECT& r = mi.rcMonitor;
        SetWindowPos(w, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    } else {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
        SetWindowLongPtrW(w, GWL_STYLE, style);
        SetWindowLongPtrW(w, GWL_EXSTYLE, GetWindowLongPtrW(w, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
        RECT r = {0, 0, static_cast<LONG>(bw), static_cast<LONG>(bh)};
        AdjustWindowRect(&r, style, FALSE);
        const RECT& work = mi.rcWork;
        int ww = r.right - r.left, wh = r.bottom - r.top;
        int x = work.left + ((work.right - work.left) - ww) / 2, y = work.top + ((work.bottom - work.top) - wh) / 2;
        if (y < work.top) y = work.top;
        SetWindowPos(w, HWND_NOTOPMOST, x, y, ww, wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }
}

const char* interval_name(UINT i) {
    switch (i) {
    case D3DPRESENT_INTERVAL_DEFAULT: return "vsync (default)";
    case D3DPRESENT_INTERVAL_ONE: return "vsync";
    case D3DPRESENT_INTERVAL_IMMEDIATE: return "no vsync";
    default: return "vsync every few refreshes";
    }
}
}  // namespace

void adjust_present(IDirect3D9* d3d, UINT adapter, D3DPRESENT_PARAMETERS* pp, HWND window, const char* when) {
    if (!pp) return;
    const Config& cfg = config();
    fo::log("display: %s: game asked for %ux%u (format %d), %s, %u Hz, %s, %u back buffer(s)", when, pp->BackBufferWidth,
            pp->BackBufferHeight, static_cast<int>(pp->BackBufferFormat), pp->Windowed ? "windowed" : "full screen", pp->FullScreen_RefreshRateInHz,
            interval_name(pp->PresentationInterval), pp->BackBufferCount);
    if (cfg.res_w != 0) {
        // Fable starts at the size in userst.ini (1024x768, 16-bit colour) whatever its Options
        // say. Use the desktop's size (or the ini's) and 32-bit colour instead. pp is the game's
        // own copy, so it sees the new size too.
        UINT w = static_cast<UINT>(cfg.res_w), h = static_cast<UINT>(cfg.res_h);
        if (cfg.res_w < 0) {
            HMONITOR mon = MonitorFromWindow(window ? window : pp->hDeviceWindow, MONITOR_DEFAULTTOPRIMARY);
            MONITORINFO mi = {sizeof mi};
            GetMonitorInfoW(mon, &mi);
            w = static_cast<UINT>(mi.rcMonitor.right - mi.rcMonitor.left);
            h = static_cast<UINT>(mi.rcMonitor.bottom - mi.rcMonitor.top);
        }
        if (w && h) { pp->BackBufferWidth = w; pp->BackBufferHeight = h; }
        if (pp->BackBufferFormat == D3DFMT_R5G6B5 || pp->BackBufferFormat == D3DFMT_X1R5G5B5 ||
            pp->BackBufferFormat == D3DFMT_A1R5G5B5 || pp->BackBufferFormat == D3DFMT_UNKNOWN)
            pp->BackBufferFormat = D3DFMT_X8R8G8B8;
        if (pp->EnableAutoDepthStencil && pp->AutoDepthStencilFormat == D3DFMT_D16) pp->AutoDepthStencilFormat = D3DFMT_D24S8;
    }
    if (cfg.window_mode != WindowMode::Fullscreen) {
        // A window instead of exclusive full screen: same back buffer, no display mode change.
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
        if (pp->SwapEffect == D3DSWAPEFFECT_FLIP) pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
        if (!pp->hDeviceWindow) pp->hDeviceWindow = window;
        // The window is restyled after the device exists (fix_window): Fable's window code
        // isn't ready for size messages while its device is being created.
        g_game_w = pp->BackBufferWidth;
        g_game_h = pp->BackBufferHeight;
    }
    if (!pp->Windowed && d3d && cfg.refresh_hz != 0) {
        // Highest rate the monitor offers at this resolution (or the one asked for in the ini).
        UINT best = 0, wanted = cfg.refresh_hz > 0 ? static_cast<UINT>(cfg.refresh_hz) : 0;
        D3DFORMAT fmt = pp->BackBufferFormat == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
        if (fmt == D3DFMT_UNKNOWN) fmt = D3DFMT_X8R8G8B8;
        const UINT n = d3d->GetAdapterModeCount(adapter, fmt);
        for (UINT i = 0; i < n; ++i) {
            D3DDISPLAYMODE m = {};
            if (FAILED(d3d->EnumAdapterModes(adapter, fmt, i, &m))) continue;
            if (m.Width != pp->BackBufferWidth || m.Height != pp->BackBufferHeight) continue;
            if (wanted ? (m.RefreshRate == wanted) : (m.RefreshRate > best)) best = m.RefreshRate;
        }
        if (best) pp->FullScreen_RefreshRateInHz = best;
        else fo::log("display: no matching refresh rate listed at %ux%u, keeping the game's", pp->BackBufferWidth, pp->BackBufferHeight);
    }
    if (cfg.vsync == 1) pp->PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (cfg.vsync == 0) pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    hud::set_screen(pp->BackBufferWidth, pp->BackBufferHeight, window ? window : pp->hDeviceWindow);
    fo::log("display: %s: using %ux%u, %u Hz, %s", when, pp->BackBufferWidth, pp->BackBufferHeight,
            pp->FullScreen_RefreshRateInHz, interval_name(pp->PresentationInterval));
}

std::atomic<bool> g_restyling{false};

void fix_window(HWND window) {
    if (!g_game_w || !g_game_h || !window) return;
    // The game never sees the size and style messages this sends (see restyling()).
    g_restyling = true;
    style_window(window, g_game_w, g_game_h);
    g_restyling = false;
    fo::log("display: window restyled for %ux%u", g_game_w, g_game_h);
}

bool restyling() { return g_restyling.load(); }

void before_present(IDirect3DDevice9* dev) {
    if (!config().low_latency || g_query_failed) return;
    IDirect3DQuery9*& q = g_query[g_slot];
    if (!q && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q))) { g_query_failed = true; return; }
    if (g_issued[g_slot]) {
        // This slot's query went in after the frame before last. Wait at most ~50 ms so a
        // lost device or a driver that never answers can't hang the game.
        LARGE_INTEGER f, t0, t;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
            QueryPerformanceCounter(&t);
            if ((t.QuadPart - t0.QuadPart) * 20 > f.QuadPart) break;
            YieldProcessor();
        }
        g_issued[g_slot] = false;
    }
}

void after_present(IDirect3DDevice9*) {
    if (config().low_latency && !g_query_failed && g_query[g_slot]) {
        g_query[g_slot]->Issue(D3DISSUE_END);
        g_issued[g_slot] = true;
        g_slot = (g_slot + 1) % kFramesAhead;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    if (g_last.QuadPart) {
        double ms = 1000.0 * static_cast<double>(now.QuadPart - g_last.QuadPart) / static_cast<double>(g_freq.QuadPart);
        if (ms > g_worst_ms.load()) g_worst_ms = ms;
    }
    g_last = now;
    if (++g_frames == 600) {
        // Once, a few seconds in: the render loop's thread and callers, to compare with the camera's.
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(__builtin_frame_address(0));
        char buf[600];
        int n = std::snprintf(buf, sizeof buf, "display: render thread %lu, present called from", GetCurrentThreadId());
        MEMORY_BASIC_INFORMATION mbi = {};
        for (int i = 0, found = 0; i < 256 && found < 14 && n < 560; ++i) {
            if (!VirtualQuery(sp + i, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) break;
            const uint32_t v = sp[i];
            if (v > 0x00401000 && v < 0x00E00000) { n += std::snprintf(buf + n, sizeof buf - n, " %08X", v); ++found; }
        }
        fo::log("%s", buf);
    }
}

void on_device_lost() {
    for (int i = 0; i < kFramesAhead; ++i) {
        if (g_query[i]) { g_query[i]->Release(); g_query[i] = nullptr; }
        g_issued[i] = false;
    }
    g_slot = 0;
}

void log_failure(const char* call, HRESULT hr) {
    fo::log("display: %s failed (0x%08lX) with these settings; try [display] window=fullscreen and refresh=game", call,
            static_cast<unsigned long>(hr));
}

void log_frame_rate() {
    static DWORD last_tick = GetTickCount();
    static unsigned last_frames = 0;
    DWORD t = GetTickCount();
    unsigned f = g_frames.load();
    if (t == last_tick) return;
    double fps = 1000.0 * (f - last_frames) / (t - last_tick);
    const double cam = 1000.0 * mouse_look::take_updates() / (t - last_tick);
    fo::log("display: %.0f frames a second (slowest frame %.0f ms), camera updates %.0f a second%s", fps, g_worst_ms.load(),
            cam, config().low_latency ? ", low latency on" : "");
    g_worst_ms = 0;
    last_tick = t;
    last_frames = f;
}
}  // namespace fo::display
