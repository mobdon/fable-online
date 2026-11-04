#include "overlay.h"
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include "../assets/fable_ui.h"
#include "../config.h"
#include "../display/camera_latch.h"
#include "../display/frame_probe.h"
#include "../display/hud_scale.h"
#include "../display/mouse_look.h"
#include "../display/present.h"
#include "../display/widescreen.h"
#include "../log.h"
#include "../menu/menu.h"
#include "../modsettings/modsettings.h"
#include "../net/chat_client.h"
#include "../ui/chat_model.h"
#include "../ui/json.h"
#include "../ui/nametags.h"
#include "../ui/trade_model.h"
#include "../ui/party_model.h"
#include "../sync.h"
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
ui::TradeModel g_trade;
std::vector<ui::TradeTarget> g_trade_targets;
std::vector<std::string> g_world_out;   // trade and party requests for the game connection
ui::PartyModel g_party;
std::vector<std::string> g_world_in;    // party frames from the game thread
game::HeroPos g_hero;
std::atomic<bool> g_chat_open{false};
std::atomic<DWORD> g_block_until{0};   // swallow keys briefly after the box closes

// Where things are in the open chat window, from the last frame drawn, for mouse clicks.
// Back-buffer pixels. Guarded by g_mu.
struct ChatHit {
    float x, y, w, h;
    enum Kind { Tab, Hero } kind;
    int tab;
    std::string name;
};
struct ChatLayout {
    bool valid = false;
    float win_x = 0, win_y = 0, win_w = 0, win_h = 0;
    float edit_x = 0, edit_y = 0, edit_w = 0, edit_h = 0;
    float screen_w = 0, screen_h = 0;
    std::vector<ChatHit> hits;
    std::vector<float> caret_x;   // x of each cursor position shown in the edit box
    size_t caret_from = 0;        // the input character caret_x[0] stands for
} g_layout;
float g_ptr_x = -1, g_ptr_y = -1;   // the chat pointer, back-buffer pixels; < 0 = not over the window

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

std::atomic<bool> g_trade_keys{false};   // the trade window or an invitation has the keyboard

bool input_blocked() {
    return menu::active() || modsettings::input_blocked() || g_chat_open.load() || g_trade_keys.load() || GetTickCount() < g_block_until.load();
}

void flush_outbox() {  // caller holds g_mu
    for (auto& line : g_chat.take_outbox()) net::chat_send(line);
    for (auto& cmd : g_chat.take_local())
        if (cmd.first == "barter") g_trade.command(cmd.second, g_trade_targets, now_s());
    for (auto& line : g_trade.take_outbox()) g_world_out.push_back(std::move(line));
    double now = now_s();
    for (auto& n : g_trade.take_notes()) g_chat.system(n.first, now, n.second);
    for (auto& line : g_chat.take_world_outbox()) g_world_out.push_back(std::move(line));
    g_chat_open = g_chat.is_open();
    g_trade_keys = g_trade.wants_keys();
    // Open chat has the mouse: Fable's camera and attacks don't see it (display/mouse_look).
    display::mouse_look::hold(g_chat_open.load(), g_hwnd);
}

// A window-message mouse position (client pixels) in back-buffer pixels. Caller holds g_mu.
bool to_backbuffer(HWND w, LONG cx, LONG cy, float& x, float& y) {
    RECT rc;
    if (!w || !GetClientRect(w, &rc) || rc.right <= 0 || rc.bottom <= 0 || g_layout.screen_w <= 0) return false;
    x = static_cast<float>(cx) * g_layout.screen_w / static_cast<float>(rc.right);
    y = static_cast<float>(cy) * g_layout.screen_h / static_cast<float>(rc.bottom);
    return true;
}

// A left click in the open chat window. Caller holds g_mu.
void chat_click(float x, float y) {
    if (!g_layout.valid) return;
    auto inside = [&](float rx, float ry, float rw, float rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; };
    for (const ChatHit& h : g_layout.hits) {
        if (!inside(h.x, h.y, h.w, h.h)) continue;
        if (h.kind == ChatHit::Tab) g_chat.set_tab(static_cast<ui::ChatTab>(h.tab));
        else g_chat.whisper_to(h.name);
        return;
    }
    if (inside(g_layout.edit_x, g_layout.edit_y, g_layout.edit_w, g_layout.edit_h) && !g_layout.caret_x.empty()) {
        size_t best = 0;
        for (size_t i = 1; i < g_layout.caret_x.size(); ++i)
            if (std::fabs(g_layout.caret_x[i] - x) < std::fabs(g_layout.caret_x[best] - x)) best = i;
        g_chat.set_cursor(g_layout.caret_from + best);
    }
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

void draw_text_line(float x, float y, float room, const std::string& s, uint32_t c, Face f);

void pump_network(double now) {
    for (auto& e : net::chat_poll()) {
        if (e.kind == net::Event::Kind::Connected) { g_chat.on_connection(true, "", now); continue; }
        if (e.kind == net::Event::Kind::Disconnected) { g_chat.on_connection(false, e.text, now); g_tags.clear(); continue; }
        json::Value m;
        if (!json::parse(e.text, m) || !m.is_object()) continue;
        if (m.str("t") == "tags") g_tags.apply(m);
        else g_chat.on_server(m, now);
    }
    for (auto& line : g_world_in) {
        json::Value m;
        if (json::parse(line, m) && m.is_object()) g_party.on_world(m, g_chat, now);
    }
    g_world_in.clear();
}

// Party members' orbs: a glow in their co-op colour over each one's stand-in, like the
// spirits of Fable's unused co-op mode (SPRITE_MULTIPLAYER_ORB).
void draw_party_orbs(const Config& cfg) {
    if (!g_party.in_party() || !g_have_view || !g_have_proj) return;
    float vp[16];
    mul(g_view, g_proj, vp);
    for (const ui::Orb& o : g_party.layout(vp, g_r.screen_w(), g_r.screen_h(), cfg.up_axis, cfg.tag_height + 0.9f)) {
        const uint32_t a = o.color >> 24, rgb = o.color & 0x00FFFFFF;
        g_r.ring(o.x, o.y, 0.0f, o.radius * 2.6f, o.radius * 2.0f, ((a * 0x50 / 255) << 24) | rgb);   // halo
        g_r.ring(o.x, o.y, 0.0f, o.radius, o.radius * 0.5f, o.color);
        g_r.ring(o.x, o.y, 0.0f, o.radius * 0.45f, o.radius * 0.3f, (a << 24) | 0xFFF6DC);       // hot centre
    }
}

// The party list on the left, under Fable's own health and Will bars: a small pewter panel
// with each member's colour, name and where they are.
void draw_party_list() {
    if (!g_party.in_party()) return;
    const auto& members = g_party.members();
    const float lh = g_r.line_height(Face::Chat), sh = g_r.line_height(Face::Small), th = g_r.line_height(Face::Title);
    const float pad = 12, row_h = lh + sh + 4, w = 210;
    const float h = pad + th + 6 + row_h * static_cast<float>(members.size()) + pad - 4;
    const float x = 16, y = std::floor(g_r.screen_h() * 0.24f);
    const uint32_t kText = 0xFFE9E4D6, kHead = 0xFFC9C0A8, kDim = 0xFF7E786C;
    g_r.panel(x, y, w, h, 0.92f);
    g_r.text(x + pad, y + pad - 2, "Party", 0xFFFFF6DC, Face::Title);
    std::string count = std::to_string(members.size()) + " of 4";
    g_r.text(x + w - pad - g_r.measure(count, Face::Small), y + pad + th - sh - 4, count, kHead, Face::Small);
    float ry = y + pad + th + 4;
    for (const ui::PartyMember& m : members) {
        const uint32_t c = ui::PartyModel::slot_color(m.slot);
        const float cx = x + pad + 7, cy = ry + lh / 2;
        g_r.ring(cx, cy, 0.0f, 9.0f, 5.0f, (0x50u << 24) | (c & 0x00FFFFFF));
        g_r.ring(cx, cy, 0.0f, 5.0f, 1.5f, m.online ? c : (0xFF000000 | ((c & 0x00FEFEFE) >> 1)));
        std::string name = m.leader ? m.name + "  (leader)" : m.name;
        draw_text_line(x + pad + 20, ry, w - pad * 2 - 20, name, m.online ? kText : kDim, Face::Chat);
        std::string where = !m.online ? "offline" : m.region.empty() ? "on the way" : m.region;
        draw_text_line(x + pad + 20, ry + lh, w - pad * 2 - 20, where, kDim, Face::Small);
        ry += row_h;
    }
}

bool g_logged_frame_camera = false;
constexpr uint32_t kTagGold = 0xFFFFD84A;      // own tag: Fable's pewter bar tinted gold
constexpr uint32_t kTagStaffRed = 0xFFD8281E;  // DEV / GM badge

void draw_tags(const Config& cfg) {
    if (!cfg.tags_enabled || !g_chat.tags_enabled() || !g_have_view || !g_have_proj) return;
    // Not over loading screens, teleports and their fades.
    if (!sync::world_shown()) return;
    float vp[16];
    mul(g_view, g_proj, vp);
    // Place tags with the camera the engine drew this frame with (after camera smoothing and
    // the shoulder offset). The view matrix seen through SetTransform is Fable's own camera
    // from its last 15-a-second update, without those, so tags placed with it stepped and sat
    // off the hero.
    float cpos[3], cfwd[3], cup[3], cview[16];
    const float* gv = &g_view._11;
    if (display::camera_latch::frame_camera(cpos, cfwd, cup, 0.25) &&
        ui::view_from_camera(cpos, cfwd, cup, gv, cview)) {
        D3DMATRIX v;
        std::memcpy(&v, cview, sizeof cview);
        mul(v, g_proj, vp);
        if (!g_logged_frame_camera) {
            g_logged_frame_camera = true;
            fo::log("tags: placed with the engine's frame camera; SetTransform view row 4 %.1f %.1f %.1f, frame camera %.1f %.1f %.1f",
                    gv[12], gv[13], gv[14], cview[12], cview[13], cview[14]);
        }
    }
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
        // Your own hero sits on a gold plate (the pewter bar, tinted), everyone else on pewter.
        g_r.bar(l.x - plate_w / 2, top, plate_w, plate_h, alpha * 0.95f, l.pvp && !l.self, true, l.self ? kTagGold : 0xFFFFFFFF);
        g_r.text(l.x - name_w / 2, top + (plate_h - name_h) / 2, l.name_line, l.name_color, Face::Title, true);
        g_r.text(l.x - g_r.measure(l.info_line, Face::Small) / 2, top + plate_h + 1, l.info_line, l.info_color, Face::Small, true);
        // Staff (from the account role the server signed, never from the client): the same
        // bar in red above the name, with DEV or GM on it.
        if (!l.badge.empty()) {
            const float bh = name_h + 6, bw_text = g_r.measure(l.badge, Face::Title), bend = 64 * bh / 33;
            const float bw = bw_text + bend * 2 + 6, by = top - bh + 1;
            g_r.bar(l.x - bw / 2, by, bw, bh, alpha * 0.95f, true, true, kTagStaffRed);
            g_r.text(l.x - bw_text / 2, by + (bh - name_h) / 2, l.badge, (l.name_color & 0xFF000000) | 0x00FFE9A8, Face::Title, true);
        }
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

    g_layout.valid = open;
    g_layout.hits.clear();
    g_layout.caret_x.clear();
    g_layout.screen_w = g_r.screen_w();
    g_layout.screen_h = g_r.screen_h();
    // Fable's HUD pieces under the open window are left out, so nothing shows through or on top.
    if (open && cfg.chat_hide_hud) display::hud::hide_under(win_x, win_y, win_w, win_h);
    else display::hud::hide_under(0, 0, 0, 0);
    if (open) {
        const uint32_t kHead = 0xFFC9C0A8, kText = 0xFFE9E4D6, kBright = 0xFFFFF6DC, kDim = 0xFF7E786C;
        g_r.window(win_x, win_y, win_w, win_h, 1.0f);
        g_layout.win_x = win_x; g_layout.win_y = win_y; g_layout.win_w = win_w; g_layout.win_h = win_h;

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
            const bool lit = !active && g_ptr_x >= sx && g_ptr_x < sx + side_w && g_ptr_y >= y && g_ptr_y < y + row_h;
            if (active || lit) g_r.bar(sx, y, side_w, row_h, active ? 1.0f : 0.45f, true, false);
            g_layout.hits.push_back({sx, y, side_w, row_h, ChatHit::Tab, t, {}});
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
                const bool lit = g_ptr_x >= sx && g_ptr_x < sx + side_w && g_ptr_y >= y && g_ptr_y < y + lh;
                draw_text_line(sx + 10, y, side_w - 12, name, target ? ui::ChatModel::kColorWhisper : lit ? kBright : kText, Face::Chat);
                g_layout.hits.push_back({sx, y, side_w, lh, ChatHit::Hero, 0, name});
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
                const bool lit = g_ptr_x >= sx && g_ptr_x < sx + side_w && g_ptr_y >= y && g_ptr_y < y + lh;
                if (lit) g_r.bar(sx, y, side_w, lh, 0.45f, true, false);
                draw_text_line(sx + 10, y, side_w - 12, t->name, ui::renown_color(t->renown) | 0xFF000000, Face::Chat);
                g_layout.hits.push_back({sx, y, side_w, lh, ChatHit::Hero, 0, t->name});
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
        g_layout.edit_x = conv_x - 8; g_layout.edit_y = edit_y; g_layout.edit_w = conv_w + 16; g_layout.edit_h = edit_h;
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
        g_layout.caret_from = from;
        for (size_t i = from; i <= to; ++i) g_layout.caret_x.push_back(lx + g_r.measure(ui::u32_to_utf8(u.substr(from, i - from))));
        if (static_cast<int>(now * 2) % 2 == 0) {
            float cx = lx + g_r.measure(ui::u32_to_utf8(u.substr(from, g_chat.cursor() - from)));
            g_r.rect(cx, iy + 1, 1, lh - 2, 0xE0FFF6DC);
        }
    }
    float y = conv_bottom - lh * static_cast<float>(phys.size() - first);
    for (size_t i = first; i < phys.size(); ++i, y += lh) g_r.text(conv_x, y, phys[i].first, phys[i].second);
}

// The pointer for the open chat window, where the Windows cursor is (Fable no longer pulls
// it back to the centre while chat has the mouse). Same arrow as the Mod settings screen.
void draw_chat_pointer() {
    g_ptr_x = g_ptr_y = -1;
    if (!g_chat.is_open() || !g_hwnd) return;
    POINT pt;
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt)) return;
    float x, y;
    if (!to_backbuffer(g_hwnd, pt.x, pt.y, x, y) || x < 0 || y < 0 || x > g_r.screen_w() || y > g_r.screen_h()) return;
    g_ptr_x = x;
    g_ptr_y = y;
    g_r.pointer(x, y, std::fmax(18.0f, g_r.line_height(Face::Chat) * 1.3f));
}

// The trade window, in the same frame as Realm chat (Lionhead's unused PC chat screen):
// your pack on the left, your offer in the middle, the other hero's offer on the right,
// each side's accept shown on Fable's pewter bar (warm once accepted). Keys are listed
// along the bottom; the model has the rules (ui/trade_model.h).
void draw_trade(double now) {
    const uint32_t kHead = 0xFFC9C0A8, kText = 0xFFE9E4D6, kBright = 0xFFFFF6DC, kDim = 0xFF7E786C, kGold = 0xFFF4E3A8;
    const float lh = g_r.line_height(Face::Chat), sh = g_r.line_height(Face::Small), th = g_r.line_height(Face::Title);
    if (g_trade.phase() == ui::TradeModel::Phase::Invited) {
        const std::string line = g_trade.partner() + " wants to trade with you";
        char tail[64];
        std::snprintf(tail, sizeof(tail), "Y: accept    N: decline    (%.0f s)", std::ceil(g_trade.invite_left(now)));
        const float w = std::max(g_r.measure(line, Face::Title), g_r.measure(tail, Face::Small)) + 64;
        const float h = th + sh + 40, x = (g_r.screen_w() - w) / 2, y = g_r.screen_h() * 0.18f;
        g_r.window(x, y, w, h, 1.0f);
        g_r.text(x + (w - g_r.measure(line, Face::Title)) / 2, y + 14, line, kBright, Face::Title);
        g_r.text(x + (w - g_r.measure(tail, Face::Small)) / 2, y + 20 + th, tail, kHead, Face::Small);
        return;
    }
    if (!g_trade.window_open()) return;

    const float pad = 18, row_h = lh + 4;
    const size_t rows = 10;
    const float col_w = std::min(260.0f, std::max(190.0f, (g_r.screen_w() - 120) / 3.0f - 24));
    const float gap = 20;
    const float win_w = pad * 2 + col_w * 3 + gap * 2;
    const float title_h = th + 10;
    const float list_h = row_h * rows;
    const float gold_h = lh + 12;
    const float win_h = pad + title_h + sh + 6 + list_h + 10 + gold_h + 10 + row_h + 8 + sh + 6 + sh + pad;
    const float wx = std::floor((g_r.screen_w() - win_w) / 2), wy = std::floor((g_r.screen_h() - win_h) / 2);
    g_r.window(wx, wy, win_w, win_h, 1.0f);

    // Title, and the purse on the right.
    g_r.text(wx + pad + 2, wy + pad - 2, "Trade with " + g_trade.partner(), kBright, Face::Title);
    const std::string purse = "Purse: " + std::to_string(g_trade.purse()) + " gold";
    g_r.text(wx + win_w - pad - g_r.measure(purse, Face::Small), wy + pad - 2 + th - sh - 2, purse, kHead, Face::Small);
    g_r.rule(wx + 10, wy + pad + title_h - 6, win_w - 20, 0.9f);

    const float top = wy + pad + title_h + 2;
    const float cx[3] = {wx + pad, wx + pad + col_w + gap, wx + pad + (col_w + gap) * 2};
    const auto focus = g_trade.focus();
    const char* heads[3] = {"Your pack", "Your offer", nullptr};
    const std::string theirs_head = g_trade.partner() + " offers";
    for (int c = 0; c < 3; ++c) {
        const bool focused = (c == 0 && focus == ui::TradeModel::Focus::Pack) || (c == 1 && focus == ui::TradeModel::Focus::Offer);
        g_r.text(cx[c] + 2, top, heads[c] ? heads[c] : theirs_head, focused ? kBright : kHead, Face::Small);
    }
    const float ly = top + sh + 6;
    for (int c = 0; c < 3; ++c) g_r.panel(cx[c] - 6, ly - 4, col_w + 12, list_h + 8, 0.9f);

    auto draw_list = [&](int c, const std::vector<ui::TradeItem>& items, bool focused) {
        // Scroll so the selection stays in view.
        size_t first = 0;
        if (focused && g_trade.selected() >= rows) first = g_trade.selected() - rows + 1;
        for (size_t i = first; i < items.size() && i < first + rows; ++i) {
            const float y = ly + row_h * static_cast<float>(i - first);
            const bool sel = focused && i == g_trade.selected();
            if (sel) g_r.bar(cx[c], y, col_w, row_h, 1.0f, true, false);
            std::string qty = items[i].qty > 1 ? std::to_string(items[i].qty) : "";
            float qw = qty.empty() ? 0 : g_r.measure(qty) + 8;
            draw_text_line(cx[c] + 8, y + (row_h - lh) / 2, col_w - 16 - qw, ui::item_label(items[i].def), sel ? kBright : kText, Face::Chat);
            if (!qty.empty()) g_r.text(cx[c] + col_w - 6 - g_r.measure(qty), y + (row_h - lh) / 2, qty, sel ? kBright : kHead);
        }
        if (items.empty()) g_r.text(cx[c] + 8, ly + (row_h - lh) / 2, c == 0 ? "Nothing you can trade" : "Nothing yet", kDim, Face::Small);
    };
    draw_list(0, g_trade.pack(), focus == ui::TradeModel::Focus::Pack);
    draw_list(1, g_trade.mine().items, focus == ui::TradeModel::Focus::Offer);
    draw_list(2, g_trade.theirs().items, false);

    // Gold under each offer: yours is an edit box like the chat's, theirs is plain.
    const float gy = ly + list_h + 10;
    {
        const bool editing = focus == ui::TradeModel::Focus::Gold;
        g_r.panel(cx[1] - 6, gy, col_w + 12, gold_h, editing ? 1.0f : 0.8f);
        const float iy = gy + (gold_h - lh) / 2;
        float x = cx[1] + 4 + g_r.text(cx[1] + 4, iy, "Gold: ", editing ? kBright : kHead);
        x += g_r.text(x, iy, g_trade.gold_text(), kGold);
        if (editing && static_cast<int>(now * 2) % 2 == 0) g_r.rect(x + 1, iy + 1, 1, lh - 2, 0xE0FFF6DC);
        g_r.text(cx[2] + 4, iy, "Gold: ", kHead);
        g_r.text(cx[2] + 4 + g_r.measure("Gold: "), iy, std::to_string(g_trade.theirs().gold), kGold);
    }

    // Accept state for each side, on the pewter bar: warm once accepted.
    const float ay = gy + gold_h + 10;
    auto accept_bar = [&](float x, bool on, const std::string& label) {
        g_r.bar(x, ay, col_w, row_h, 1.0f, on, true);
        g_r.text(x + (col_w - g_r.measure(label)) / 2, ay + (row_h - lh) / 2, label, on ? kBright : kText);
    };
    std::string mine_label;
    if (g_trade.accepted_me()) mine_label = "You accepted";
    else if (g_trade.can_accept(now)) mine_label = "A: accept";
    else {
        char b[48];
        std::snprintf(b, sizeof(b), "Look it over... %.0f", std::ceil(std::max(0.1, g_trade.settle_left(now))));
        mine_label = b;
    }
    accept_bar(cx[1], g_trade.accepted_me(), mine_label);
    accept_bar(cx[2], g_trade.accepted_them(), g_trade.accepted_them() ? g_trade.partner() + " accepted" : "Not accepted yet");

    // A refusal from the server, then the keys.
    const float ny = ay + row_h + 8;
    if (!g_trade.problem().empty()) draw_text_line(wx + pad, ny, win_w - pad * 2, g_trade.problem(), ui::TradeModel::kColorError, Face::Small);
    else if (g_trade.accepted_me() && !g_trade.accepted_them())
        g_r.text(wx + pad, ny, "Waiting for " + g_trade.partner() + " to accept.", kHead, Face::Small);
    else if (g_trade.accepted_them() && !g_trade.accepted_me())
        g_r.text(wx + pad, ny, g_trade.partner() + " has accepted this offer.", kHead, Face::Small);
    const std::string keys = "Tab: switch    Up/Down: pick    Enter: offer / take back    Left/Right: how many    A: accept    Esc: cancel";
    draw_text_line(wx + pad, ny + sh + 6, win_w - pad * 2, keys, kDim, Face::Small);
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
        g_tags.set_self(cfg.tags_self, g_chat.me().empty() ? cfg.name : g_chat.me(), std::max(0, sync::own_renown()),
                        g_hero.x, g_hero.y, g_hero.z, g_chat.my_staff());
    if (!g_r.begin(dev, art, cfg.font_px)) return;
    draw_crosshair(cfg);
    draw_tags(cfg);
    draw_party_orbs(cfg);
    draw_party_list();
    if (cfg.chat_enabled) draw_chat(cfg, now);
    g_trade.step(now);
    flush_outbox();
    draw_trade(now);
    modsettings::draw_hud(g_r, g_hero.valid, g_hero.x, g_hero.y, g_hero.z, g_hero.area);
    modsettings::draw_pause_button(g_r, g_hero.valid);
    modsettings::draw(g_r);
    if (cfg.chat_enabled && !modsettings::is_open()) draw_chat_pointer();   // on top of everything
    g_r.end();
}

// Over the start-up menu only the Mod settings screen is drawn (chat and tags wait).
void draw_settings_only(IDirect3DDevice9* dev) {
    if (!modsettings::is_open()) return;
    const assets::UiArt* art = g_art.load();
    if (!art) return;
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_r.begin(dev, art, config().font_px)) return;
    modsettings::draw(g_r);
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
            draw_settings_only(dev);
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
    if (modsettings::on_message(h, msg, w, l)) return 0;
    if (menu::active() && menu::on_message(h, msg, w, l)) return 0;
    if (cfg.chat_enabled && cfg.overlay_enabled) {
        static wchar_t high_surrogate = 0;
        switch (msg) {
        case WM_KEYDOWN: {
            std::lock_guard<std::mutex> lock(g_mu);
            double now = now_s();
            if (!g_chat.is_open() && g_trade.wants_keys()) {
                ui::Key k;
                if (map_key(w, k)) g_trade.key(k, now);
                if (!g_trade.wants_keys()) g_block_until = GetTickCount() + 200;
                flush_outbox();
                return 0;
            }
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
            if (g_chat.take_settings_request()) {
                if (g_chat.is_open()) g_chat.close();
                modsettings::open(modsettings::kExtras);
            }
            if (!g_chat.is_open()) g_block_until = GetTickCount() + 200;
            flush_outbox();
            return 0;
        }
        case WM_KEYUP:
        case WM_CHAR:
            if (!input_blocked()) break;
            if (msg == WM_CHAR && !g_chat_open && g_trade_keys) {
                std::lock_guard<std::mutex> lock(g_mu);
                const wchar_t c = static_cast<wchar_t>(w);
                if (c >= 0x20) g_trade.character(static_cast<char32_t>(c), now_s());
                flush_outbox();
                return 0;
            }
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
            if (g_chat.is_open()) { g_chat.close(); flush_outbox(); }
            break;
        }
        case WM_SETCURSOR:
            // Our own pointer is drawn while chat is open; keep the Windows arrow out of the way.
            if (g_chat_open && LOWORD(l) == HTCLIENT) { SetCursor(nullptr); return TRUE; }
            break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            if (!g_chat_open) break;
            std::lock_guard<std::mutex> lock(g_mu);
            float x, y;
            if (to_backbuffer(h, static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l)), x, y)) chat_click(x, y);
            flush_outbox();
            return 0;
        }
        case WM_MOUSEWHEEL: {
            if (!g_chat_open) break;
            std::lock_guard<std::mutex> lock(g_mu);
            g_chat.scroll_by(GET_WHEEL_DELTA_WPARAM(w) > 0 ? 3 : -3);
            return 0;
        }
        case WM_MOUSEMOVE: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            if (g_chat_open) return 0;   // chat has the mouse; Fable doesn't see it
            break;
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
    if (SUCCEEDED(hr) && data && is_mouse(dev) && modsettings::is_open()) {
        modsettings::on_mouse_state(data, size);   // the settings screen has the mouse, nothing under it
        std::memset(data, 0, size);
    } else if (SUCCEEDED(hr) && data && is_mouse(dev) && menu::active()) {
        menu::on_mouse_state(data, size);   // the menu reads the mouse, Fable's menu underneath doesn't
        std::memset(data, 0, size);
    } else if (SUCCEEDED(hr) && data && is_mouse(dev) && g_chat_open) {
        std::memset(data, 0, size);   // chat has the mouse
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
    if (SUCCEEDED(hr) && inout && is_mouse(dev) && modsettings::is_open()) {
        if (rg && !(flags & 0x1 /* DIGDD_PEEK */)) modsettings::on_mouse_data(rg, cb, *inout);
        *inout = 0;
    } else if (SUCCEEDED(hr) && inout && is_mouse(dev) && menu::active()) {
        if (rg && !(flags & 0x1 /* DIGDD_PEEK */)) menu::on_mouse_data(rg, cb, *inout);
        *inout = 0;
    } else if (SUCCEEDED(hr) && inout && is_mouse(dev) && g_chat_open) {
        *inout = 0;   // chat has the mouse
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

void on_trade_frame(const std::string& text) {
    json::Value m;
    if (!json::parse(text, m) || !m.is_object()) return;
    std::lock_guard<std::mutex> lock(g_mu);
    g_trade.on_frame(m, now_s());
    flush_outbox();
}

void on_world_disconnected() {
    std::lock_guard<std::mutex> lock(g_mu);
    g_trade.on_disconnected(now_s());
    g_world_out.clear();
    flush_outbox();
}

void set_trade_targets(std::vector<ui::TradeTarget> targets) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_trade_targets = std::move(targets);
}

void post_world_frame(const std::string& json) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_world_in.size() < 256) g_world_in.push_back(json);
}

// Trade and party requests share one queue; either name drains it.
std::vector<std::string> take_world_requests() { return take_world_outbox(); }

std::vector<std::string> take_world_outbox() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<std::string> out;
    out.swap(g_world_out);
    return out;
}

void set_party_marks(std::vector<ui::PartyMark> marks) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_party.set_marks(std::move(marks));
}

void clear_party() {
    // Goes through the same queue as the server's frames, so it lands after any already queued.
    post_world_frame("{\"t\":\"party\",\"id\":null}");
}

}  // namespace fo::overlay
