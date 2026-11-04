#include "hud_scale.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../config.h"
#include "../log.h"
#include "../overlay/patch.h"
#include "hud_patch.h"

namespace fo::display::hud {
namespace {
constexpr int kGetRenderTarget = 38, kSetViewport = 47, kSetScissorRect = 75, kDrawPrimitiveUP = 83,
              kDrawIndexedPrimitiveUP = 84, kGetFVF = 90, kSetVertexShaderConstantF = 94;
using SetVertexShaderConstantF_t = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetScissorRect_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*);
using SetViewport_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using DrawPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT,
                                                  const void*, UINT);

SetVertexShaderConstantF_t o_SetVertexShaderConstantF = nullptr;
SetScissorRect_t o_SetScissorRect = nullptr;
SetViewport_t o_SetViewport = nullptr;
DrawPrimitiveUP_t o_DrawPrimitiveUP = nullptr;
DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP = nullptr;

char g_status[200] = "not installed";
volatile int32_t* g_virtual = nullptr;      // [0] width, [1] height inside the code block
std::atomic<float> g_scale{1.0f};
UINT g_w = 0, g_h = 0, g_vw = 0, g_vh = 0;
std::atomic<unsigned> g_c20_fixed{0}, g_scissor_fixed{0}, g_viewport_fixed{0}, g_rhw_fixed{0};

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

// True when the device draws to a target the size of the screen (the back buffer), not to
// a shadow map, reflection or other render texture.
bool drawing_to_screen(IDirect3DDevice9* d) {
    IDirect3DSurface9* rt = nullptr;
    if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return false;
    D3DSURFACE_DESC desc = {};
    const bool ok = SUCCEEDED(rt->GetDesc(&desc)) && desc.Width == g_w && desc.Height == g_h;
    rt->Release();
    return ok;
}

// Clipping rectangles from the 2D layer are in virtual pixels.
HRESULT WINAPI hk_SetScissorRect(IDirect3DDevice9* d, const RECT* r) {
    if (r && active() && r->right <= static_cast<LONG>(g_vw) + 1 && r->bottom <= static_cast<LONG>(g_vh) + 1 &&
        drawing_to_screen(d)) {
        const float s = g_scale.load();
        RECT x = {static_cast<LONG>(std::floor(r->left * s)), static_cast<LONG>(std::floor(r->top * s)),
                  static_cast<LONG>(std::ceil(r->right * s)), static_cast<LONG>(std::ceil(r->bottom * s))};
        if (x.right > static_cast<LONG>(g_w)) x.right = static_cast<LONG>(g_w);
        if (x.bottom > static_cast<LONG>(g_h)) x.bottom = static_cast<LONG>(g_h);
        ++g_scissor_fixed;
        return o_SetScissorRect(d, &x);
    }
    return o_SetScissorRect(d, r);
}

// 2D viewports (menus, text boxes, videos, loading screens) come from the layout in virtual
// pixels too: Fable sets the virtual size + 1 for full-screen 2D, and + 2 for videos.
// Viewports already the screen's size are 3D.
constexpr UINT kViewportSlack = 4;
HRESULT WINAPI hk_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    if (v && active() && !(v->Width == g_w && v->Height == g_h) && v->X + v->Width <= g_vw + kViewportSlack &&
        v->Y + v->Height <= g_vh + kViewportSlack && drawing_to_screen(d)) {
        const float s = g_scale.load();
        D3DVIEWPORT9 x = *v;
        const DWORD l = static_cast<DWORD>(std::floor(v->X * s)), t = static_cast<DWORD>(std::floor(v->Y * s));
        DWORD r = static_cast<DWORD>(std::ceil((v->X + v->Width) * s)), b = static_cast<DWORD>(std::ceil((v->Y + v->Height) * s));
        if (r > g_w) r = g_w;
        if (b > g_h) b = g_h;
        if (r > l && b > t) {
            x.X = l; x.Y = t; x.Width = r - l; x.Height = b - t;
            ++g_viewport_fixed;
            return o_SetViewport(d, &x);
        }
    }
    return o_SetViewport(d, v);
}

// Pre-transformed (XYZRHW) 2D from Fable's own code, such as loading-screen text, comes in
// layout pixels and skips the shaders, so its positions are scaled here. Only draws whose
// vertices all fit the virtual screen are touched: real-pixel full-screen quads are left
// alone, and so is the mod's own overlay (called from outside Fable.exe).
bool from_fable(void* ret) {
    const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
    return a >= 0x00400000 && a < 0x01500000;
}

UINT vertex_count(D3DPRIMITIVETYPE t, UINT prims) {
    switch (t) {
    case D3DPT_POINTLIST: return prims;
    case D3DPT_LINELIST: return prims * 2;
    case D3DPT_LINESTRIP: return prims + 1;
    case D3DPT_TRIANGLELIST: return prims * 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return prims + 2;
    default: return 0;
    }
}

// Returns scaled copies of `count` vertices from `first`, or nullptr to draw them as they are.
const void* scaled_rhw(IDirect3DDevice9* d, const void* data, UINT first, UINT count, UINT stride) {
    if (!data || !count || stride < 16 || count > 65536 || !active()) return nullptr;
    DWORD fvf = 0;
    if (FAILED(d->GetFVF(&fvf)) || (fvf & D3DFVF_POSITION_MASK) != D3DFVF_XYZRHW) return nullptr;
    const uint8_t* src = static_cast<const uint8_t*>(data) + static_cast<size_t>(first) * stride;
    const float lim_x = static_cast<float>(g_vw) + 4.0f, lim_y = static_cast<float>(g_vh) + 4.0f;
    for (UINT i = 0; i < count; ++i) {
        const float* p = reinterpret_cast<const float*>(src + static_cast<size_t>(i) * stride);
        if (p[0] > lim_x || p[1] > lim_y) return nullptr;
    }
    if (!drawing_to_screen(d)) return nullptr;
    static thread_local std::vector<uint8_t> buf;
    const size_t bytes = static_cast<size_t>(first + count) * stride;
    if (buf.size() < bytes) buf.resize(bytes);
    std::memcpy(buf.data() + static_cast<size_t>(first) * stride, src, static_cast<size_t>(count) * stride);
    const float s = g_scale.load();
    for (UINT i = 0; i < count; ++i) {
        float* p = reinterpret_cast<float*>(buf.data() + static_cast<size_t>(first + i) * stride);
        // Keep Fable's half-pixel offset: (x + 0.5) * s - 0.5.
        p[0] = (p[0] + 0.5f) * s - 0.5f;
        p[1] = (p[1] + 0.5f) * s - 0.5f;
    }
    ++g_rhw_fixed;
    return buf.data();
}

HRESULT WINAPI hk_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT prims, const void* data, UINT stride) {
    if (from_fable(__builtin_return_address(0)))
        if (const void* v = scaled_rhw(d, data, 0, vertex_count(t, prims), stride)) return o_DrawPrimitiveUP(d, t, prims, v, stride);
    return o_DrawPrimitiveUP(d, t, prims, data, stride);
}

HRESULT WINAPI hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT min_index, UINT verts, UINT prims,
                                         const void* idx, D3DFORMAT fmt, const void* data, UINT stride) {
    if (from_fable(__builtin_return_address(0)))
        if (const void* v = scaled_rhw(d, data, min_index, verts, stride))
            return o_DrawIndexedPrimitiveUP(d, t, min_index, verts, prims, idx, fmt, v, stride);
    return o_DrawIndexedPrimitiveUP(d, t, min_index, verts, prims, idx, fmt, data, stride);
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
    // The cursor is left alone: Fable's CMouseDX reads movement as GetCursorPos minus the
    // window centre and warps the cursor back with SetCursorPos every update, so remapping
    // those calls only rounded small moves away and could pin the warp at the screen edge.
    std::snprintf(g_status, sizeof g_status, "HUD scaling ready");
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
    if (auto o = fo::patch::vtable(dev, kSetViewport, reinterpret_cast<void*>(hk_SetViewport)))
        o_SetViewport = reinterpret_cast<SetViewport_t>(o);
    if (auto o = fo::patch::vtable(dev, kDrawPrimitiveUP, reinterpret_cast<void*>(hk_DrawPrimitiveUP)))
        o_DrawPrimitiveUP = reinterpret_cast<DrawPrimitiveUP_t>(o);
    if (auto o = fo::patch::vtable(dev, kDrawIndexedPrimitiveUP, reinterpret_cast<void*>(hk_DrawIndexedPrimitiveUP)))
        o_DrawIndexedPrimitiveUP = reinterpret_cast<DrawIndexedPrimitiveUP_t>(o);
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
        fo::log("display: HUD laid out for %ux%u, drawn at %ux%u (x%.2f); 2D constants fixed %u, clip rects %u, 2D viewports %u, screen-space draws %u",
                g_vw, g_vh, g_w, g_h, static_cast<double>(g_scale.load()), g_c20_fixed.load(), g_scissor_fixed.load(),
                g_viewport_fixed.load(), g_rhw_fixed.load());
}
}  // namespace fo::display::hud
