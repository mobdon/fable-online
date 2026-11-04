#include "overlay.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include "../assets/fable_ui.h"
#include "../config.h"
#include "../display/frame_probe.h"
#include "../display/hud_scale.h"
#include "../display/present.h"
#include "../display/widescreen.h"
#include "../log.h"
#include "../menu/menu.h"
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
const GUID kSysMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

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
std::atomic<void*> g_mice[4] = {};
std::atomic<bool> g_mouse_hooked{false};
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

bool input_blocked() { return menu::active() || g_chat_open.load() || GetTickCount() < g_block_until.load(); }

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
    s.line_height = (name_h + 8 + info_h + 6) / 2;  // a plate and its info line: keeps stacked tags apart
    // Each name sits on Fable's pewter scroll bar (warm when flagged for PvP), with the title
    // and renown underneath in its small Tahoma, outlined like the game's floating text.
    for (const auto& l : g_tags.layout(vp, g_r.screen_w(), g_r.screen_h(), s)) {
        const float alpha = static_cast<float>(l.name_color >> 24) / 255.0f;
        const float plate_h = name_h + 8, end_w = 64 * plate_h / 33;   // bar()'s end width at this height
        const float name_w = g_r.measure(l.name_line, Face::Title);
        const float plate_w = name_w + end_w * 2 + 10;                 // the name sits on the plain middle of the bar
        const float top = l.y - plate_h - info_h - 2;
        g_r.bar(l.x - plate_w / 2, top, plate_w, plate_h, alpha * 0.95f, l.pvp, true);
        g_r.text(l.x - name_w / 2, top + (plate_h - name_h) / 2, l.name_line, l.name_color, Face::Title, true);
        g_r.text(l.x - g_r.measure(l.info_line, Face::Small) / 2, top + plate_h + 1, l.info_line, l.info_color, Face::Small, true);
    }
}

// Realm chat, laid out like Lionhead's unused PC chat screen (PC_MSN_SCREEN): one framed
// window with a list down the left (channels, the heroes you whisper with, heroes nearby),
// the conversation on the right and the edit box under it. Closed, only the newest lines
// stay up and fade.
void draw_text_line(float x, float y, float room, const std::string& s, uint32_t c, Face f) {
    if (g_r.measure(s, f) <= room) { g_r.text(x, y, s, c, f); return; }
    std::u32string u = ui::utf8_to_u32(s);
    while (!u.empty() && g_r.measure(ui::u32_to_utf8(u) + "...", f) > room) u.pop_back();
    g_r.text(x, y, ui::u32_to_utf8(u) + "...", c, f);
}

void draw_chat(const Config& cfg, double now) {
    const float lh = g_r.line_height(Face::Chat), sh = g_r.line_height(Face::Small), th = g_r.line_height(Face::Title);
    const bool open = g_chat.is_open();
    const float pad = 16;
    const float win_w = std::min(780.0f, std::max(480.0f, g_r.screen_w() * 0.58f));
    const float side_w = 168;                      // the list column
    const float title_h = th + 10, edit_h = lh + 12;
    // Tall enough for the channels and a few contacts; the conversation gets the rest.
    int rows = cfg.chat_rows;
    if (open) {
        const size_t whispers = std::min<size_t>(g_chat.contacts().size(), 3);
        const size_t near_n = std::min<size_t>(std::max<size_t>(g_tags.size(), 1), 4);
        const float side = (sh + 3) + static_cast<float>(ui::ChatTab::Count) * (lh + 4) + 6 +
                           (whispers ? sh + 3 + lh * whispers + 6 : 0) + sh + 3 + lh * near_n;
        while (lh * rows + 10 + edit_h < side && rows < 30) ++rows;
    }
    const float win_h = pad + title_h + lh * rows + 10 + edit_h + pad;
    const float win_x = 16, win_y = g_r.screen_h() - 40 - win_h;
    const float conv_x = open ? win_x + pad + side_w + 16 : win_x + pad;
    const float conv_w = win_x + win_w - pad - conv_x;
    const float edit_y = win_y + win_h - pad - edit_h;
    const float conv_bottom = edit_y - 8;

    std::vector<std::pair<std::string, uint32_t>> phys;
    for (const auto& v : g_chat.visible(static_cast<size_t>(rows), now)) {
        uint32_t a = static_cast<uint32_t>(v.alpha * ((v.line->color >> 24) & 0xFF));
        uint32_t c = (a << 24) | (v.line->color & 0x00FFFFFF);
        for (auto& part : g_r.wrap(v.line->text, conv_w - 4)) phys.emplace_back(std::move(part), c);
    }
    size_t first = phys.size() > static_cast<size_t>(rows) ? phys.size() - rows : 0;

    if (open) {
        const uint32_t kHead = 0xFFC9C0A8, kText = 0xFFE9E4D6, kBright = 0xFFFFF6DC, kDim = 0xFF7E786C;
        g_r.window(win_x, win_y, win_w, win_h, 1.0f);

        // Title bar: the window's name, and the connection on the right.
        const float ty = win_y + pad - 2;
        g_r.text(win_x + pad + 2, ty, "Realm Chat", kBright, Face::Title);
        std::string status = net::chat_connected() ? "Online" : "Offline - reconnecting";
        g_r.text(win_x + win_w - pad - g_r.measure(status, Face::Small), ty + th - sh - 2, status,
                 net::chat_connected() ? kHead : ui::ChatModel::kColorError, Face::Small);
        g_r.rule(win_x + 10, win_y + pad + title_h - 6, win_w - 20, 0.9f);

        // Left column: channels (the tabs), then whispers and nearby heroes like a contact list.
        const float sx = win_x + pad, top = win_y + pad + title_h + 2, bottom = win_y + win_h - pad;
        float y = top;
        auto heading = [&](const char* name) {
            g_r.text(sx + 2, y, name, kHead, Face::Small);
            y += sh + 3;
        };
        heading("Channels");
        const float row_h = lh + 4;
        for (int t = 0; t < static_cast<int>(ui::ChatTab::Count); ++t) {
            auto tab = static_cast<ui::ChatTab>(t);
            bool active = g_chat.tab() == tab, fresh = g_chat.unread(tab);
            if (active) g_r.bar(sx, y, side_w, row_h, 1.0f, true, false);
            g_r.text(sx + 10, y + (row_h - lh) / 2, ui::tab_name(tab), active ? kBright : fresh ? 0xFFF4E3A8 : kText);
            if (fresh && !active) g_r.text(sx + side_w - 14, y + (row_h - lh) / 2, "*", 0xFFF4E3A8);
            y += row_h;
        }
        y += 6;
        auto room_for_row = [&] { return y + lh <= bottom; };
        if (!g_chat.contacts().empty() && y + sh + 3 + lh <= bottom) {
            heading("Whispers");
            for (const auto& name : g_chat.contacts()) {
                if (!room_for_row()) break;
                bool target = g_chat.channel_label() == "To " + name;
                draw_text_line(sx + 10, y, side_w - 12, name, target ? ui::ChatModel::kColorWhisper : kText, Face::Chat);
                y += lh;
            }
            y += 6;
        }
        std::vector<const ui::Tag*> nearby;
        for (const auto& kv : g_tags.tags()) nearby.push_back(&kv.second);
        std::sort(nearby.begin(), nearby.end(), [](const ui::Tag* a, const ui::Tag* b) { return a->renown > b->renown; });
        if (y + sh + 3 + lh <= bottom) {
            heading(nearby.empty() ? "Nearby: nobody" : "Nearby");
            size_t shown = 0;
            for (const ui::Tag* t : nearby) {
                if (!room_for_row()) break;
                if (shown + 1 < nearby.size() && y + 2 * lh > bottom) {   // last row: say how many more
                    g_r.text(sx + 10, y, "and " + std::to_string(nearby.size() - shown) + " more", kDim, Face::Small);
                    break;
                }
                draw_text_line(sx + 10, y, side_w - 12, t->name, ui::renown_color(t->renown) | 0xFF000000, Face::Chat);
                y += lh;
                ++shown;
            }
        }

        // Divider between the list and the conversation.
        g_r.rect(sx + side_w + 7, top, 1, bottom - top, 0x90938D7E);
        g_r.rect(sx + side_w + 8, top, 1, bottom - top, 0x60000000);

        // Scrolled up: a note at the top of the conversation.
        if (g_chat.scrolled() > 0)
            g_r.text(conv_x + conv_w - g_r.measure("PgDn: newer", Face::Small), top, "PgDn: newer", kDim, Face::Small);

        // Edit box: an inset frame under the conversation, like PC_MSN_EDIT_BOX.
        g_r.panel(conv_x - 8, edit_y, conv_w + 16, edit_h, 0.95f);
        const float iy = edit_y + (edit_h - lh) / 2, ix = conv_x + 2;
        std::string label = g_chat.channel_label() + ": ";
        float lx = ix + g_r.text(ix, iy, label, net::chat_connected() ? kHead : kDim);
        // Long input scrolls sideways so the cursor stays in the box.
        std::u32string u = ui::utf8_to_u32(g_chat.input_utf8());
        const float room = conv_x + conv_w - 4 - lx;
        size_t from = 0;
        while (from < g_chat.cursor() && g_r.measure(ui::u32_to_utf8(u.substr(from, g_chat.cursor() - from))) > room) ++from;
        size_t to = from;
        while (to < u.size() && g_r.measure(ui::u32_to_utf8(u.substr(from, to + 1 - from))) <= room) ++to;
        g_r.text(lx, iy, ui::u32_to_utf8(u.substr(from, to - from)), kBright);
        if (static_cast<int>(now * 2) % 2 == 0) {
            float cx = lx + g_r.measure(ui::u32_to_utf8(u.substr(from, g_chat.cursor() - from)));
            g_r.rect(cx, iy + 1, 1, lh - 2, 0xE0FFF6DC);
        }
    }
    float y = conv_bottom - lh * static_cast<float>(phys.size() - first);
    for (size_t i = first; i < phys.size(); ++i, y += lh) g_r.text(conv_x, y, phys[i].first, phys[i].second);
}

// A small mark at the centre of the view, in the warm parchment-gold of Fable's menu text
// over a soft dark halo so it reads on snow and on night scenes alike. Sized with the screen
// height, so it looks the same at 1080p and at 4K.
void draw_crosshair(const Config& cfg) {
    if (cfg.crosshair == Crosshair::Off || !g_hero.valid) return;
    const float k = g_r.screen_h() / 900.0f;
    const float cx = std::floor(g_r.screen_w() * 0.5f) + 0.5f, cy = std::floor(g_r.screen_h() * 0.5f) + 0.5f;
    constexpr uint32_t kGold = 0xD8E9D6A0, kHalo = 0x70000000;
    switch (cfg.crosshair) {
    case Crosshair::Ring:
        g_r.ring(cx, cy, 7.9f * k, 9.6f * k, 0.8f * k, kHalo);
        g_r.ring(cx, cy, 8.45f * k, 9.05f * k, 0.5f * k, kGold);
        g_r.ring(cx, cy, 0.0f, 2.2f * k, 1.0f * k, kHalo);
        g_r.ring(cx, cy, 0.0f, 1.3f * k, 0.6f * k, kGold);
        break;
    case Crosshair::Dot:
        g_r.ring(cx, cy, 0.0f, 3.2f * k, 1.2f * k, kHalo);
        g_r.ring(cx, cy, 0.0f, 2.0f * k, 0.7f * k, kGold);
        break;
    case Crosshair::Cross: {
        const float gap = 3.5f * k, len = 7.0f * k, t = 1.4f * k, f = 0.7f * k;
        for (int halo = 1; halo >= 0; --halo) {
            const float e = halo ? 1.0f * k : 0.0f;
            const uint32_t c = halo ? kHalo : kGold;
            g_r.soft_rect(cx - gap - len - e, cy - t / 2 - e, len + 2 * e, t + 2 * e, f, c);
            g_r.soft_rect(cx + gap - e, cy - t / 2 - e, len + 2 * e, t + 2 * e, f, c);
            g_r.soft_rect(cx - t / 2 - e, cy - gap - len - e, t + 2 * e, len + 2 * e, f, c);
            g_r.soft_rect(cx - t / 2 - e, cy + gap - e, t + 2 * e, len + 2 * e, f, c);
        }
        break;
    }
    case Crosshair::Off: break;
    }
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
    draw_crosshair(cfg);
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
    if (menu::active() && !g_drawn) {
        // The start-up menu covers the whole screen; chat and tags wait until it closes.
        if (g_no_backbuffer > 300 || targets_backbuffer(dev)) {
            menu::draw(dev);
            g_drawn = g_present_seen;
            g_no_backbuffer = 0;
        } else {
            ++g_no_backbuffer;
        }
        return o_EndScene(dev);
    }
    if (config().overlay_enabled && !g_drawn) {
        display::probe::note("EndScene (overlay draws after this if the target is the back buffer)");
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
    if (config().frame_probe) display::probe::on_present(dev);
    g_present_seen = true;
    g_drawn = false;
    display::before_present(dev);
    HRESULT hr = o_Present(dev, a, b, w, r);
    display::after_present(dev);
    return hr;
}

HRESULT WINAPI hk_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp) {
    g_r.on_lost();
    menu::on_device_lost();
    display::on_device_lost();
    {
        IDirect3D9* d3d = nullptr;
        D3DDEVICE_CREATION_PARAMETERS cp = {};
        dev->GetCreationParameters(&cp);
        dev->GetDirect3D(&d3d);
        display::adjust_present(d3d, cp.AdapterOrdinal, pp, cp.hFocusWindow ? cp.hFocusWindow : g_hwnd, "reset");
        if (d3d) d3d->Release();
    }
    HRESULT hr = o_Reset(dev, pp);
    if (SUCCEEDED(hr)) display::fix_window(g_hwnd);
    else display::log_failure("Reset", hr);
    if (SUCCEEDED(hr) && pp) display::on_backbuffer(pp->BackBufferWidth, pp->BackBufferHeight, pp->hDeviceWindow ? pp->hDeviceWindow : g_hwnd);
    return hr;
}

HRESULT WINAPI hk_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) {
    // Keep the camera: the view matrix set while a perspective projection is active.
    // (The HUD sets an orthographic projection later in the frame; ignore that one.)
    // Games set view and projection in either order, so pair a perspective projection with
    // the latest view whichever comes first.
    if (m) {
        display::probe::on_transform(st, m);
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
    case VK_TAB: k = (GetKeyState(VK_SHIFT) & 0x8000) ? ui::Key::BackTab : ui::Key::Tab; return true;
    }
    return false;
}

LRESULT CALLBACK hk_WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const Config& cfg = config();
    if (display::restyling()) return DefWindowProcW(h, msg, w, l);
    if (menu::active() && menu::on_message(h, msg, w, l)) return 0;
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
    return CallWindowProcW(o_WndProc, h, msg, w, display::hud::map_mouse(msg, l));
}

bool is_keyboard(void* dev) {
    for (auto& k : g_keyboards) if (k.load() == dev) return true;
    return false;
}

bool is_mouse(void* dev) {
    for (auto& m : g_mice) if (m.load() == dev) return true;
    return false;
}

/** Mouse buttons as Fable reads them through DirectInput (bit n = button n), for sync. */
std::atomic<unsigned> g_mouse_buttons{0};

HRESULT WINAPI hk_GetDeviceState(void* dev, DWORD size, LPVOID data) {
    HRESULT hr = o_GetDeviceState(dev, size, data);
    if (SUCCEEDED(hr) && data && size >= 16 && is_mouse(dev)) {
        // DIMOUSESTATE: lX, lY, lZ, then rgbButtons[4] at byte 12.
        const unsigned char* b = static_cast<const unsigned char*>(data) + 12;
        unsigned m = 0;
        for (int i = 0; i < 4; ++i)
            if (b[i] & 0x80) m |= 1u << i;
        g_mouse_buttons = m;
    }
    if (SUCCEEDED(hr) && data && is_mouse(dev) && menu::active()) {
        menu::on_mouse_state(data, size);   // the menu reads the mouse, Fable's menu underneath doesn't
        std::memset(data, 0, size);
    }
    if (SUCCEEDED(hr) && data && is_keyboard(dev) && input_blocked()) std::memset(data, 0, size);
    return hr;
}

HRESULT WINAPI hk_GetDeviceData(void* dev, DWORD cb, void* rg, LPDWORD inout, DWORD flags) {
    HRESULT hr = o_GetDeviceData(dev, cb, rg, inout, flags);
    if (SUCCEEDED(hr) && inout && rg && cb >= 8 && is_mouse(dev) && !(flags & 0x1 /* DIGDD_PEEK */)) {
        // Buffered DIDEVICEOBJECTDATA: dwOfs, dwData, ... ; DIMOFS_BUTTON0..3 are offsets 12..15.
        unsigned m = g_mouse_buttons.load();
        const unsigned char* p = static_cast<const unsigned char*>(rg);
        for (DWORD i = 0; i < *inout; ++i, p += cb) {
            DWORD ofs = 0, val = 0;
            std::memcpy(&ofs, p, 4);
            std::memcpy(&val, p + 4, 4);
            if (ofs >= 12 && ofs <= 15) {
                unsigned bit = 1u << (ofs - 12);
                m = (val & 0x80) ? (m | bit) : (m & ~bit);
            }
        }
        g_mouse_buttons = m;
    }
    if (SUCCEEDED(hr) && inout && is_mouse(dev) && menu::active()) {
        if (rg && !(flags & 0x1 /* DIGDD_PEEK */)) menu::on_mouse_data(rg, cb, *inout);
        *inout = 0;
    }
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
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, kSysMouse)) {
        // Keyboard and mouse devices share the same vtable, so these slots are the same hooks.
        for (auto& m : g_mice) { void* e = nullptr; if (m.compare_exchange_strong(e, *out)) break; }
        if (auto o = patch::vtable(*out, kDIDev_GetDeviceState, reinterpret_cast<void*>(hk_GetDeviceState))) o_GetDeviceState = reinterpret_cast<GetDeviceState_t>(o);
        if (auto o = patch::vtable(*out, kDIDev_GetDeviceData, reinterpret_cast<void*>(hk_GetDeviceData))) o_GetDeviceData = reinterpret_cast<GetDeviceData_t>(o);
        g_mouse_hooked = o_GetDeviceState && o_GetDeviceData;
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
    display::hud::hook_device(dev);
    if (config().frame_probe) display::probe::hook(dev);
    g_device_hooked = o_EndScene && o_Present && o_Reset && o_SetTransform;
}

HRESULT WINAPI hk_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND focus, DWORD flags,
                               D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out) {
    display::adjust_present(d3d, adapter, pp, focus ? focus : (pp ? pp->hDeviceWindow : nullptr), "start");
    HRESULT hr = o_CreateDevice(d3d, adapter, type, focus, flags, pp, out);
    if (FAILED(hr)) display::log_failure("CreateDevice", hr);
    if (SUCCEEDED(hr) && out && *out) hook_device(*out, focus ? focus : (pp ? pp->hDeviceWindow : nullptr));
    if (SUCCEEDED(hr)) display::fix_window(g_hwnd ? g_hwnd : focus);   // after hook_window, so the game misses the resize
    if (SUCCEEDED(hr) && pp) display::on_backbuffer(pp->BackBufferWidth, pp->BackBufferHeight, pp->hDeviceWindow ? pp->hDeviceWindow : focus);
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
    fo::log("overlay: DirectInput mouse %s", g_mouse_hooked ? "hooked" : "not hooked (the start-up menu uses window mouse messages only)");
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
bool input_blocked_now() { return input_blocked(); }
unsigned mouse_buttons() { return g_mouse_buttons.load(); }

}  // namespace fo::overlay
