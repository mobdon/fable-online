#include "hud_scale.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../config.h"
#include "../log.h"
#include "../overlay/patch.h"
#include "hud_patch.h"

namespace fo::display::hud {
namespace {
constexpr int kSetScissorRect = 75, kSetVertexShaderConstantF = 94;
using SetVertexShaderConstantF_t = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetScissorRect_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
using GetCursorPos_t = BOOL(WINAPI*)(LPPOINT);
using SetCursorPos_t = BOOL(WINAPI*)(int, int);

SetVertexShaderConstantF_t o_SetVertexShaderConstantF = nullptr;
SetScissorRect_t o_SetScissorRect = nullptr;
GetCursorPos_t o_GetCursorPos = nullptr;
SetCursorPos_t o_SetCursorPos = nullptr;

char g_status[200] = "not installed";
volatile int32_t* g_virtual = nullptr;      // [0] width, [1] height inside the code block
std::atomic<float> g_scale{1.0f};
UINT g_w = 0, g_h = 0, g_vw = 0, g_vh = 0;
std::atomic<unsigned> g_c20_fixed{0}, g_scissor_fixed{0};

bool active() { return g_virtual && g_virtual[0] != 0 && g_scale.load() > 1.001f; }

HWND g_window = nullptr;
HWND game_window() { return g_window; }

// The game's 2D shader turns pixels into clip space with c20 = (2/w, 2/h, -1, 1). When it was
// built from the real size, swap in the virtual one so the layout fills the screen.
HRESULT WINAPI hk_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT start, const float* c, UINT n) {
    if (c && active() && start <= 20 && start + n > 20 && g_w && g_h) {
        const float* r = c + (20 - start) * 4;
        const float rx = 2.0f / static_cast<float>(g_w), ry = 2.0f / static_cast<float>(g_h);
        if (std::fabs(r[0] - rx) < rx * 0.01f && std::fabs(r[1] - ry) < ry * 0.01f && r[2] == -1.0f && r[3] == 1.0f) {
            float buf[4 * 32];
            if (n <= 32) {
                std::memcpy(buf, c, n * 16);
                float* w = buf + (20 - start) * 4;
                w[0] = 2.0f / static_cast<float>(g_vw);
                w[1] = 2.0f / static_cast<float>(g_vh);
                ++g_c20_fixed;
                return o_SetVertexShaderConstantF(d, start, buf, n);
            }
        }
    }
    return o_SetVertexShaderConstantF(d, start, c, n);
}

// Clipping rectangles from the 2D layer are in virtual pixels.
HRESULT WINAPI hk_SetScissorRect(IDirect3DDevice9* d, const RECT* r) {
    if (r && active() && r->right <= static_cast<LONG>(g_vw) && r->bottom <= static_cast<LONG>(g_vh)) {
        const float s = g_scale.load();
        RECT x = {static_cast<LONG>(std::floor(r->left * s)), static_cast<LONG>(std::floor(r->top * s)),
                  static_cast<LONG>(std::ceil(r->right * s)), static_cast<LONG>(std::ceil(r->bottom * s))};
        ++g_scissor_fixed;
        return o_SetScissorRect(d, &x);
    }
    return o_SetScissorRect(d, r);
}

POINT client_origin() {
    POINT o = {0, 0};
    if (HWND w = game_window()) ClientToScreen(w, &o);
    return o;
}

BOOL WINAPI hk_GetCursorPos(LPPOINT p) {
    BOOL ok = o_GetCursorPos(p);
    if (ok && p && active()) {
        const float s = g_scale.load();
        POINT o = client_origin();
        p->x = o.x + static_cast<LONG>(std::lround((p->x - o.x) / s));
        p->y = o.y + static_cast<LONG>(std::lround((p->y - o.y) / s));
    }
    return ok;
}

BOOL WINAPI hk_SetCursorPos(int x, int y) {
    if (active()) {
        const float s = g_scale.load();
        POINT o = client_origin();
        x = o.x + static_cast<int>(std::lround((x - o.x) * s));
        y = o.y + static_cast<int>(std::lround((y - o.y) * s));
    }
    return o_SetCursorPos(x, y);
}

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
}  // namespace

void install() {
    const Config& cfg = config();
    if (cfg.hud_scale < 0) { std::snprintf(g_status, sizeof g_status, "off in fable_online.ini"); return; }
    ScreenSizeSite site;
    if (!readable(site.func, sizeof kScreenSizeBytes) ||
        std::memcmp(reinterpret_cast<const void*>(site.func), kScreenSizeBytes, sizeof kScreenSizeBytes) != 0) {
        std::snprintf(g_status, sizeof g_status, "screen-size code not where expected, HUD left at Fable's size");
        return;
    }
    void* mem = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    const uint32_t cave = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(mem));
    auto code = build_screen_size_cave(cave, site);
    std::memcpy(mem, code.data(), code.size());
    g_virtual = reinterpret_cast<volatile int32_t*>(cave + kVirtualSizeOffset);
    auto jump = build_screen_size_jump(cave, site);
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(site.func);
    if (!VirtualProtect(at, jump.size(), PAGE_EXECUTE_READWRITE, &old)) { g_virtual = nullptr; return; }
    std::memcpy(at, jump.data(), jump.size());
    VirtualProtect(at, jump.size(), old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, jump.size());
    HMODULE exe = GetModuleHandleW(nullptr);
    if (auto o = fo::patch::iat(exe, "user32.dll", "GetCursorPos", reinterpret_cast<void*>(hk_GetCursorPos)))
        o_GetCursorPos = reinterpret_cast<GetCursorPos_t>(o);
    if (auto o = fo::patch::iat(exe, "user32.dll", "SetCursorPos", reinterpret_cast<void*>(hk_SetCursorPos)))
        o_SetCursorPos = reinterpret_cast<SetCursorPos_t>(o);
    std::snprintf(g_status, sizeof g_status, "HUD scaling ready (cursor hooks %s/%s)", o_GetCursorPos ? "yes" : "no",
                  o_SetCursorPos ? "yes" : "no");
}

void set_screen(UINT w, UINT h, HWND window) {
    if (window) g_window = window;
    if (!g_virtual || !w || !h) return;
    const float s = hud_scale_for(config().hud_scale, h);
    g_w = w;
    g_h = h;
    g_vw = static_cast<UINT>(std::lround(w / s));
    g_vh = static_cast<UINT>(std::lround(h / s));
    g_scale = static_cast<float>(w) / static_cast<float>(g_vw);   // exact for x; y is within a pixel
    if (s > 1.001f) {
        g_virtual[1] = static_cast<int32_t>(g_vh);
        g_virtual[0] = static_cast<int32_t>(g_vw);
    } else {
        g_virtual[0] = 0;
    }
}

void hook_device(IDirect3DDevice9* dev) {
    if (!g_virtual) return;
    if (auto o = fo::patch::vtable(dev, kSetVertexShaderConstantF, reinterpret_cast<void*>(hk_SetVertexShaderConstantF)))
        o_SetVertexShaderConstantF = reinterpret_cast<SetVertexShaderConstantF_t>(o);
    if (auto o = fo::patch::vtable(dev, kSetScissorRect, reinterpret_cast<void*>(hk_SetScissorRect)))
        o_SetScissorRect = reinterpret_cast<SetScissorRect_t>(o);
}

LPARAM map_mouse(UINT msg, LPARAM l) {
    if (!active()) return l;
    if (msg < WM_MOUSEFIRST || msg > WM_MOUSELAST || msg == WM_MOUSEWHEEL) return l;
    const float s = g_scale.load();
    int x = static_cast<short>(LOWORD(l)), y = static_cast<short>(HIWORD(l));
    x = static_cast<int>(std::lround(x / s));
    y = static_cast<int>(std::lround(y / s));
    return MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
}

float scale() { return active() ? g_scale.load() : 1.0f; }

void log_status() {
    fo::log("display: %s", g_status);
    if (active())
        fo::log("display: HUD laid out for %ux%u, drawn at %ux%u (x%.2f); 2D constants fixed %u, clip rects %u", g_vw, g_vh,
                g_w, g_h, static_cast<double>(g_scale.load()), g_c20_fixed.load(), g_scissor_fixed.load());
}
}  // namespace fo::display::hud
