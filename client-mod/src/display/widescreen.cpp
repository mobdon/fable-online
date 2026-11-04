#include "widescreen.h"
#include <cstdio>
#include <cstring>
#include <string>
#include "../config.h"
#include "../log.h"
#include "camera_patch.h"
#include "hud_scale.h"
#include "camera_latch.h"
#include "mouse_look.h"
#include "target_colour.h"
#include "../game/pause_menu.h"

namespace fo::display {
namespace {
// Fable's HUD and menus are laid out on a 640x480 page and scaled to the screen through these
// two floats in .data (read by 0x41CC14 and the GUI's coordinate helpers near 0x52E470).
// Scaling x and y by different amounts is what stretches the HUD on a wide screen. Logged
// for now; the HUD fix needs to know how the 2D layer is drawn (see frame_probe.h).
float* const kGuiPageW = reinterpret_cast<float*>(0x01375CD4);
float* const kGuiPageH = reinterpret_cast<float*>(0x01375CD8);

char g_status[256] = "not installed";
bool g_camera_ok = false;
UINT g_w = 0, g_h = 0;

// True when [addr, addr+n) is committed memory we can read (MinGW has no __try).
bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool same(uint32_t addr, const uint8_t* bytes, size_t n) {
    return readable(addr, n) && std::memcmp(reinterpret_cast<const void*>(addr), bytes, n) == 0;
}

bool write_code(uint32_t addr, const std::vector<uint8_t>& bytes) {
    DWORD old = 0;
    void* p = reinterpret_cast<void*>(addr);
    if (!VirtualProtect(p, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(p, bytes.data(), bytes.size());
    VirtualProtect(p, bytes.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, bytes.size());
    return true;
}
}  // namespace

void install() {
    const Config& cfg = config();
    if (!cfg.widescreen) { std::snprintf(g_status, sizeof g_status, "off in fable_online.ini"); return; }
    CameraSite site;
    if (!same(site.hook, kHookBytes, sizeof kHookBytes) || !same(site.auto_fov, kAutoBytes, sizeof kAutoBytes) ||
        !same(site.done, kDoneBytes, sizeof kDoneBytes)) {
        std::snprintf(g_status, sizeof g_status, "camera code not where expected (a different Fable.exe?), field of view left alone");
        return;
    }
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) { std::snprintf(g_status, sizeof g_status, "no memory for the camera patch"); return; }
    const uint32_t cave = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));
    auto code = build_camera_cave(cave, site, cfg.fov_scale);
    std::memcpy(mem, code.data(), code.size());
    FlushInstructionCache(GetCurrentProcess(), mem, code.size());
    if (!write_code(site.hook, build_camera_jump(cave, site))) {
        std::snprintf(g_status, sizeof g_status, "couldn't write the camera patch");
        return;
    }
    g_camera_ok = true;
    std::snprintf(g_status, sizeof g_status, "Hor+ field of view on (fov_scale %.2f)", static_cast<double>(cfg.fov_scale));
}

void on_backbuffer(UINT w, UINT h, HWND window) {
    if ((!w || !h) && window) {   // windowed mode can leave the size to the window
        RECT r = {};
        if (GetClientRect(window, &r)) { w = static_cast<UINT>(r.right - r.left); h = static_cast<UINT>(r.bottom - r.top); }
    }
    if (!w || !h) return;
    g_w = w;
    g_h = h;
}

void log_status() {
    fo::log("display: %s", g_status);
    hud::log_status();
    fo::log("display: %s", mouse_look::status());
    fo::log("display: %s", mouse_look::center_status());
    fo::log("display: %s", mouse_look::aim_status());
    fo::log("display: %s", mouse_look::assist_status());
    fo::log("display: %s", camera_latch::status());
    fo::log("display: %s", target_colour::status());
    fo::log("display: %s", fo::game::pause_menu::status());
    if (g_w && g_h) {
        const float a = static_cast<float>(g_w) / static_cast<float>(g_h);
        fo::log("display: back buffer %ux%u (aspect %.3f)%s", g_w, g_h, a,
                a < 1.34f ? "  <- 4:3 or narrower: pick your screen's own resolution in Fable's Options for widescreen" : "");
        if (g_camera_ok && a > 1.34f) {
            const float fov = 1.2f;   // a typical Fable camera angle, for the comparison only
            Scales o = fable_original(fov, a, 1), n = hor_plus(fov, a, 1, config().fov_scale);
            fo::log("display: horizontal view %.0f%% of Fable's own at this shape, vertical %.0f%%",
                    100.0f * o.xs / n.xs, 100.0f * o.ys / n.ys);
        }
    }
    if (readable(0x01375CD4, 8))
        fo::log("display: HUD page %.0fx%.0f, scaled to the screen by the game",
                static_cast<double>(*kGuiPageW), static_cast<double>(*kGuiPageH));
}
}  // namespace fo::display
