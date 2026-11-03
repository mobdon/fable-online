#include "overlay.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include "../assets/fable_ui.h"
#include "../config.h"
#include "../log.h"
#include "../net/chat_client.h"
#include "../ui/chat_model.h"
#include "../ui/json.h"
#include "../ui/nametags.h"
#include "patch.h"
#include "renderer.h"

namespace fo::overlay {
namespace {

// ---- vtable slots (d3d9.h / dinput.h declaration order) ---------------------
constexpr int kD3D9_CreateDevice = 16;
constexpr int kDev_Reset = 16, kDev_Present = 17, kDev_EndScene = 42, kDev_SetTransform = 44;
constexpr int kDI_CreateDevice = 3;
constexpr int kDIDev_GetDeviceState = 9, kDIDev_GetDeviceData = 10;
const GUID kSysKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

using Direct3DCreate9_t = IDirect3D9*(WINAPI*)(UINT);
using CreateDevice_t = HRESULT(WINAPI*)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using Reset_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
using Present_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using EndScene_t = HRESULT(WINAPI*)(IDirect3DDevice9*);
using SetTransform_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DTRANSFORMSTATETYPE, const D3DMATRIX*);
using DirectInput8Create_t = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
using DICreateDevice_t = HRESULT(WINAPI*)(void*, REFGUID, void**, LPUNKNOWN);
using GetDeviceState_t = HRESULT(WINAPI*)(void*, DWORD, LPVOID);
using GetDeviceData_t = HRESULT(WINAPI*)(void*, DWORD, void*, LPDWORD, DWORD);

Direct3DCreate9_t o_Direct3DCreate9 = nullptr;
CreateDevice_t o_CreateDevice = nullptr;
Reset_t o_Reset = nullptr;
Present_t o_Present = nullptr;
EndScene_t o_EndScene = nullptr;
SetTransform_t o_SetTransform = nullptr;
DirectInput8Create_t o_DirectInput8Create = nullptr;
DICreateDevice_t o_DICreateDevice = nullptr;
GetDeviceState_t o_GetDeviceState = nullptr;
GetDeviceData_t o_GetDeviceData = nullptr;
WNDPROC o_WndProc = nullptr;

// What install_early managed, logged later (no file I/O inside DllMain).
bool g_iat_d3d = false, g_iat_di = false;

std::atomic<bool> g_device_hooked{false};
std::atomic<bool> g_keyboard_hooked{false};
std::atomic<void*> g_keyboards[4] = {};
HWND g_hwnd = nullptr;

// ---- shared state (game thread + mod thread) -----------------------------
std::mutex g_mu;
ui::ChatModel g_chat;
ui::TagStore g_tags;
game::HeroPos g_hero;
std::atomic<bool> g_chat_open{false};
std::atomic<DWORD> g_block_until{0};   // swallow keys briefly after the box closes

// Fable's fonts and frame art, read once by the mod thread; the render thread waits for it.
std::atomic<const assets::UiArt*> g_art{nullptr};

// Render-thread-only state.
Renderer g_r;
D3DMATRIX g_view = {}, g_proj = {}, g_pending_view = {};
bool g_have_pending_view = false;
bool g_have_view = false, g_have_proj = false, g_proj_is_persp = false;
bool g_drawn = false, g_present_seen = false;
unsigned g_frames = 0, g_view_sets = 0, g_proj_sets = 0, g_no_backbuffer = 0;
double g_last_frame = 0;

double now_s() {
    static LARGE_INTEGER f = {}, t0 = {};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
}

bool input_blocked() { return g_chat_open.load() || GetTickCount() < g_block_until.load(); }

void flush_outbox() {  // caller holds g_mu
    for (auto& line : g_chat.take_outbox()) net::chat_send(line);
    g_chat_open = g_chat.is_open();
}

// ---- drawing ---------------------------------------------------------------
void mul(const D3DMATRIX& a, const D3DMATRIX& b, float out[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            out[i * 4 + j] = s;
        }
}

void pump_network(double now) {
    for (auto& e : net::chat_poll()) {
        if (e.kind == net::Event::Kind::Connected) { g_chat.on_connection(true, "", now); continue; }
        if (e.kind == net::Event::Kind::Disconnected) { g_chat.on_connection(false, e.text, now); g_tags.clear(); continue; }
        json::Value m;
        if (!json::parse(e.text, m) || !m.is_object()) continue;
        if (m.str("t") == "tags") g_tags.apply(m);
        else g_chat.on_server(m, now);
    }
}

void draw_tags(const Config& cfg) {
    if (!cfg.tags_enabled || !g_chat.tags_enabled() || !g_have_view || !g_have_proj) return;
    float vp[16];
    mul(g_view, g_proj, vp);
    ui::TagSettings s;
    s.head_height = cfg.tag_height;
    s.up_axis = cfg.up_axis;
    s.full_until = cfg.tag_full;
    s.hidden_from = cfg.tag_hidden;
    const float name_h = g_r.line_height(Face::Title), info_h = g_r.line_height(Face::Small);
    s.line_height = (name_h + info_h) / 2;
    // Name in Fable's caption face (Arial 21), title and renown underneath in its small Tahoma,
    // both outlined in black like the game's own floating text.
    for (const auto& l : g_tags.layout(vp, g_r.screen_w(), g_r.screen_h(), s)) {
        float top = l.y - name_h - info_h;
        g_r.text(l.x - g_r.measure(l.name_line, Face::Title) / 2, top, l.name_line, l.name_color, Face::Title, true);
        g_r.text(l.x - g_r.measure(l.info_line, Face::Small) / 2, top + name_h, l.info_line, l.info_color, Face::Small, true);
    }
}

void draw_chat(const Config& cfg, double now) {
    const float lh = g_r.line_height(Face::Chat);
    const float pad = 14;
    const float w = g_r.screen_w() * 0.42f > 620 ? 620 : g_r.screen_w() * 0.42f;
    const int rows = cfg.chat_rows;
    const float box_x = 16, box_bottom = g_r.screen_h() - 40;
    const float input_h = lh + 10;
    const float box_h = lh * rows + input_h + pad * 2 + 4;
    const float box_y = box_bottom - box_h;
    const float x = box_x + pad;
    const float input_y = box_bottom - pad - lh - 2;
    const float log_bottom = input_y - 10;

    std::vector<std::pair<std::string, uint32_t>> phys;
    for (const auto& v : g_chat.visible(static_cast<size_t>(rows), now)) {
        uint32_t a = static_cast<uint32_t>(v.alpha * ((v.line->color >> 24) & 0xFF));
        uint32_t c = (a << 24) | (v.line->color & 0x00FFFFFF);
        for (auto& part : g_r.wrap(v.line->text, w - pad * 2)) phys.emplace_back(std::move(part), c);
    }
    size_t first = phys.size() > static_cast<size_t>(rows) ? phys.size() - rows : 0;
    if (g_chat.is_open()) {
        g_r.panel(box_x, box_y, w, box_h, 1.0f);
        g_r.rule(box_x + 10, input_y - 9, w - 20, 0.9f);
        std::string label = g_chat.channel_label() + ": ";
        float lx = x + g_r.text(x, input_y, label, net::chat_connected() ? 0xFFC9C0A8 : 0xFF7E786C);
        // Long input scrolls sideways so the cursor stays in the box.
        std::u32string u = ui::utf8_to_u32(g_chat.input_utf8());
        const float room = box_x + w - pad - 4 - lx;
        size_t from = 0;
        while (from < g_chat.cursor() && g_r.measure(ui::u32_to_utf8(u.substr(from, g_chat.cursor() - from))) > room) ++from;
        size_t to = from;
        while (to < u.size() && g_r.measure(ui::u32_to_utf8(u.substr(from, to + 1 - from))) <= room) ++to;
        g_r.text(lx, input_y, ui::u32_to_utf8(u.substr(from, to - from)), 0xFFFFF6DC);
        if (static_cast<int>(now * 2) % 2 == 0) {
            float cx = lx + g_r.measure(ui::u32_to_utf8(u.substr(from, g_chat.cursor() - from)));
            g_r.rect(cx, input_y + 1, 1, lh - 2, 0xE0FFF6DC);
        }
        if (!net::chat_connected())
            g_r.text(box_x + 4, box_y - lh - 4, "The chat server can't be reached. Trying again...", ui::ChatModel::kColorError);
    }
    float y = log_bottom - lh * static_cast<float>(phys.size() - first);
    for (size_t i = first; i < phys.size(); ++i, y += lh) g_r.text(x, y, phys[i].first, phys[i].second);
}

void draw(IDirect3DDevice9* dev) {
    const Config& cfg = config();
    const assets::UiArt* art = g_art.load();
    if (!art) return;  // still reading the game's fonts (a fraction of a second at start-up)
    std::lock_guard<std::mutex> lock(g_mu);
    double now = now_s();
    float dt = g_last_frame > 0 ? static_cast<float>(now - g_last_frame) : 0.0f;
    g_last_frame = now;
    ++g_frames;
    pump_network(now);
    flush_outbox();
    g_tags.step(dt > 0.25f ? 0.25f : dt);
    if (g_hero.valid)
        g_tags.set_self(cfg.tags_self, g_chat.me().empty() ? cfg.name : g_chat.me(), 0, g_hero.x, g_hero.y, g_hero.z);
    if (!g_r.begin(dev, art, cfg.font_px)) return;
    draw_tags(cfg);
    if (cfg.chat_enabled) draw_chat(cfg, now);
    g_r.end();
}

bool targets_backbuffer(IDirect3DDevice9* dev) {
    IDirect3DSurface9 *rt = nullptr, *bb = nullptr;
    bool same = false;
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)))
        same = rt == bb;
    if (rt) rt->Release();
    if (bb) bb->Release();
    return same;
}

// ---- Direct3D hooks ------------------------------------------------------------
HRESULT WINAPI hk_EndScene(IDirect3DDevice9* dev) {
    if (config().overlay_enabled && !g_drawn) {
        // Draw once a frame, into the scene that ends up on screen. If the game never
        // ends a scene on the back buffer (some post-processing paths), stop checking.
        if (g_no_backbuffer > 300 || targets_backbuffer(dev)) {
            draw(dev);
            g_drawn = g_present_seen;
            g_no_backbuffer = 0;
        } else {
            ++g_no_backbuffer;
        }
    }
    return o_EndScene(dev);
}

HRESULT WINAPI hk_Present(IDirect3DDevice9* dev, const RECT* a, const RECT* b, HWND w, const RGNDATA* r) {
    g_present_seen = true;
    g_drawn = false;
    return o_Present(dev, a, b, w, r);
}

HRESULT WINAPI hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    g_r.on_lost();
    return o_Reset(dev, pp);
}

HRESULT WINAPI hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) {
    // Keep the camera: the view matrix set while a perspective projection is active.
    // (The HUD sets an orthographic projection later in the frame; ignore that one.)
    // Games set view and projection in either order, so pair a perspective projection with
    // the latest view whichever comes first.
    if (m) {
        if (st == D3DTS_PROJECTION) {
            g_proj_is_persp = m->_34 != 0.0f && m->_44 == 0.0f;
            if (g_proj_is_persp) {
                g_proj = *m; g_have_proj = true; ++g_proj_sets;
                if (g_have_pending_view) { g_view = g_pending_view; g_have_view = true; }
            }
        } else if (st == D3DTS_VIEW) {
            g_pending_view = *m; g_have_pending_view = true;
            if (g_proj_is_persp) { g_view = *m; g_have_view = true; }
            ++g_view_sets;
        }
    }
    return o_SetTransform(dev, st, m);
}

// ---- window and DirectInput hooks ---------------------------------------------------
bool map_key(WPARAM vk, ui::Key& k) {
    switch (vk) {
    case VK_RETURN: k = ui::Key::Enter; return true;
    case VK_ESCAPE: k = ui::Key::Escape; return true;
    case VK_BACK: k = ui::Key::Backspace; return true;
    case VK_DELETE: k = ui::Key::Delete; return true;
    case VK_LEFT: k = ui::Key::Left; return true;
    case VK_RIGHT: k = ui::Key::Right; return true;
    case VK_HOME: k = ui::Key::Home; return true;
    case VK_END: k = ui::Key::End; return true;
    case VK_UP: k = ui::Key::Up; return true;
    case VK_DOWN: k = ui::Key::Down; return true;
    case VK_PRIOR: k = ui::Key::PageUp; return true;
    case VK_NEXT: k = ui::Key::PageDown; return true;
    case VK_TAB: k = ui::Key::Tab; return true;
    }
    return false;
}

LRESULT CALLBACK hk_WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const Config& cfg = config();
    if (cfg.chat_enabled && cfg.overlay_enabled) {
        static wchar_t high_surrogate = 0;
        switch (msg) {
        case WM_KEYDOWN: {
            std::lock_guard<std::mutex> lock(g_mu);
            double now = now_s();
            if (!g_chat.is_open()) {
                if (static_cast<int>(w) == cfg.open_key && !(GetKeyState(VK_MENU) & 0x8000)) {
                    g_chat.open();
                    flush_outbox();
                    return 0;
                }
                break;
            }
            ui::Key k;
            if (map_key(w, k)) g_chat.key(k, now);
            if (!g_chat.is_open()) g_block_until = GetTickCount() + 200;
            flush_outbox();
            return 0;
        }
        case WM_KEYUP:
        case WM_CHAR:
            if (!input_blocked()) break;
            if (msg == WM_CHAR && g_chat_open) {
                std::lock_guard<std::mutex> lock(g_mu);
                wchar_t c = static_cast<wchar_t>(w);
                if (c >= 0xD800 && c < 0xDC00) { high_surrogate = c; return 0; }
                char32_t cp = c;
                if (c >= 0xDC00 && c < 0xE000) { cp = high_surrogate ? 0x10000 + ((high_surrogate - 0xD800) << 10) + (c - 0xDC00) : U'?'; high_surrogate = 0; }
                g_chat.character(cp, now_s());
            }
            return 0;
        case WM_KILLFOCUS: {
            std::lock_guard<std::mutex> lock(g_mu);  // alt-tab closes the box so the game isn't stuck
            if (g_chat.is_open()) { g_chat.close(); g_chat_open = false; }
            break;
        }
        }
    }
    return CallWindowProcW(o_WndProc, h, msg, w, l);
}

bool is_keyboard(void* dev) {
    for (auto& k : g_keyboards) if (k.load() == dev) return true;
    return false;
}

HRESULT WINAPI hk_GetDeviceState(void* dev, DWORD size, LPVOID data) {
    HRESULT hr = o_GetDeviceState(dev, size, data);
    if (SUCCEEDED(hr) && data && is_keyboard(dev) && input_blocked()) std::memset(data, 0, size);
    return hr;
}

HRESULT WINAPI hk_GetDeviceData(void* dev, DWORD cb, void* rg, LPDWORD inout, DWORD flags) {
    HRESULT hr = o_GetDeviceData(dev, cb, rg, inout, flags);
    if (SUCCEEDED(hr) && inout && is_keyboard(dev) && input_blocked()) *inout = 0;  // read and thrown away
    return hr;
}

HRESULT WINAPI hk_DICreateDevice(void* di, REFGUID guid, void** out, LPUNKNOWN unk) {
    HRESULT hr = o_DICreateDevice(di, guid, out, unk);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, kSysKeyboard)) {
        for (auto& k : g_keyboards) { void* e = nullptr; if (k.compare_exchange_strong(e, *out)) break; }
        if (auto o = patch::vtable(*out, kDIDev_GetDeviceState, reinterpret_cast<void*>(hk_GetDeviceState))) o_GetDeviceState = reinterpret_cast<GetDeviceState_t>(o);
        if (auto o = patch::vtable(*out, kDIDev_GetDeviceData, reinterpret_cast<void*>(hk_GetDeviceData))) o_GetDeviceData = reinterpret_cast<GetDeviceData_t>(o);
        g_keyboard_hooked = o_GetDeviceState && o_GetDeviceData;
    }
    return hr;
}

HRESULT WINAPI hk_DirectInput8Create(HINSTANCE inst, DWORD ver, REFIID iid, LPVOID* out, LPUNKNOWN unk) {
    HRESULT hr = o_DirectInput8Create(inst, ver, iid, out, unk);
    if (SUCCEEDED(hr) && out && *out)
        if (auto o = patch::vtable(*out, kDI_CreateDevice, reinterpret_cast<void*>(hk_DICreateDevice))) o_DICreateDevice = reinterpret_cast<DICreateDevice_t>(o);
    return hr;
}

void hook_window(HWND w) {
    if (!w || g_hwnd) return;
    g_hwnd = w;
    o_WndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(w, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hk_WndProc)));
}

void hook_device(IDirect3DDevice9* dev, HWND w) {
    if (auto o = patch::vtable(dev, kDev_Reset, reinterpret_cast<void*>(hk_Reset))) o_Reset = reinterpret_cast<Reset_t>(o);
    if (auto o = patch::vtable(dev, kDev_Present, reinterpret_cast<void*>(hk_Present))) o_Present = reinterpret_cast<Present_t>(o);
    if (auto o = patch::vtable(dev, kDev_EndScene, reinterpret_cast<void*>(hk_EndScene))) o_EndScene = reinterpret_cast<EndScene_t>(o);
    if (auto o = patch::vtable(dev, kDev_SetTransform, reinterpret_cast<void*>(hk_SetTransform))) o_SetTransform = reinterpret_cast<SetTransform_t>(o);
    hook_window(w);
    g_device_hooked = o_EndScene && o_Present && o_Reset && o_SetTransform;
}

HRESULT WINAPI hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                               D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (SUCCEEDED(hr) && out && *out) hook_device(*out, focus ? focus : (pp ? pp->hDeviceWindow : nullptr));
    return hr;
}

IDirect3D9* WINAPI hk_Direct3DCreate9(UINT sdk) {
    IDirect3D9* d3d = o_Direct3DCreate9(sdk);
    if (d3d)
        if (auto o = patch::vtable(d3d, kD3D9_CreateDevice, reinterpret_cast<void*>(hk_CreateDevice))) o_CreateDevice = reinterpret_cast<CreateDevice_t>(o);
    return d3d;
}

// Fallback when Fable didn't import Direct3DCreate9 directly: make a throwaway device and
// patch the vtable it shares with the game's device.
BOOL CALLBACK find_game_window(HWND w, LPARAM out) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) {
        *reinterpret_cast<HWND*>(out) = w;
        return FALSE;
    }
    return TRUE;
}

void fallback_dummy_device() {
    HMODULE d3d9 = LoadLibraryW(L"d3d9.dll");
    auto create = d3d9 ? reinterpret_cast<Direct3DCreate9_t>(GetProcAddress(d3d9, "Direct3DCreate9")) : nullptr;
    if (!create) { fo::log("overlay: no d3d9.dll, overlay off"); return; }
    HWND game = nullptr;
    EnumWindows(find_game_window, reinterpret_cast<LPARAM>(&game));
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FableOnlineDummy";
    RegisterClassW(&wc);
    HWND tmp = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPED, 0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = tmp;
    if (d3d && SUCCEEDED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, tmp, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        hook_device(dev, game);
        dev->Release();
        fo::log("overlay: hooked via a throwaway device (window %s)", game ? "found" : "NOT found: typing won't work");
    } else {
        fo::log("overlay: throwaway device failed, overlay off");
    }
    if (d3d) d3d->Release();
    DestroyWindow(tmp);
}
}  // namespace

void install_early() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (auto o = patch::iat(exe, "d3d9.dll", "Direct3DCreate9", reinterpret_cast<void*>(hk_Direct3DCreate9))) {
        o_Direct3DCreate9 = reinterpret_cast<Direct3DCreate9_t>(o);
        g_iat_d3d = true;
    }
    if (auto o = patch::iat(exe, "dinput8.dll", "DirectInput8Create", reinterpret_cast<void*>(hk_DirectInput8Create))) {
        o_DirectInput8Create = reinterpret_cast<DirectInput8Create_t>(o);
        g_iat_di = true;
    }
}

void install_late() {
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/"));
        auto* art = new assets::UiArt(assets::load_ui(dir));  // lives for the whole process
        fo::log("overlay: game art: %s", art->report.c_str());
        g_art = art;
    }
    fo::log("overlay: import hooks: Direct3DCreate9 %s, DirectInput8Create %s",
            g_iat_d3d ? "yes" : "NOT IMPORTED", g_iat_di ? "yes" : "NOT IMPORTED");
    for (int i = 0; i < 300 && !g_device_hooked; ++i) Sleep(100);  // wait up to 30 s for the game's device
    if (!g_device_hooked) {
        fo::log("overlay: the game's device wasn't caught at creation; trying the fallback");
        fallback_dummy_device();
    }
    fo::log("overlay: device %s, window %s, keyboard %s", g_device_hooked ? "hooked" : "NOT hooked",
            g_hwnd ? "hooked" : "NOT hooked", g_keyboard_hooked ? "hooked" : "not hooked (typing may also move the hero)");
}

void set_hero(const game::HeroPos& pos) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_hero = pos;
}

void log_stats() {
    // Racy reads of render-thread counters; fine for a log line.
    size_t tags;
    { std::lock_guard<std::mutex> lock(g_mu); tags = g_tags.size(); }
    fo::log("overlay: %u frames, camera view sets %u, perspective sets %u, %u name tags in range%s",
            g_frames, g_view_sets, g_proj_sets, static_cast<unsigned>(tags),
            (g_frames && !g_view_sets) ? "  <- no camera matrix seen: name tags can't be placed yet" : "");
}
}  // namespace fo::overlay
