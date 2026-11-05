#include "menu.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../../../shared/art/big.h"
#include "../config.h"
#include "../log.h"
#include "menu_model.h"
#include "../modsettings/modsettings.h"

namespace fo::menu {
namespace {

constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };

// Palette sampled from Fable's front end (tools/re-notes/04-frontend-art.md).
constexpr uint32_t kText = 0xFFE9E4D6, kTextLit = 0xFFFFF6DC, kMuted = 0xFFC9C0A8, kLabel = 0xFFB3AA92;

HMODULE g_self = nullptr;
std::mutex g_mu;            // guards g_model and the pointer state
Model g_model;
std::atomic<bool> g_active{true};
std::atomic<bool> g_disabled{false};
float g_px = -1, g_py = -1;  // pointer in back-buffer pixels
DWORD g_last_wm_mouse = 0;  // last WM_MOUSEMOVE; while recent, DirectInput deltas are ignored
bool g_dinput_button = false;
float g_client_to_bb_x = 1, g_client_to_bb_y = 1;

// ---- render-thread state ----------------------------------------------------
struct Tex { IDirect3DTexture9* t = nullptr; float w = 0, h = 0, u1 = 1, v1 = 1; };
IDirect3DDevice9* g_dev = nullptr;
IDirect3DStateBlock9* g_saved = nullptr;
bool g_art_tried = false, g_art_ok = false, g_fonts_ok = false;
Tex g_bg[6], g_beam[6], g_btn_off, g_btn_on, g_hero_bg, g_pointer, g_white;
std::map<std::string, Tex> g_text;   // baked strings
float g_text_scale = 0;
std::vector<Vertex> g_verts;
IDirect3DTexture9* g_bound = nullptr;
double g_t0 = 0;
std::string g_status;
unsigned g_status_serial = 0;
double g_status_since = 0;
unsigned g_frames = 0;

double now_s() {
    static LARGE_INTEGER f = {}, t0 = {};
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<double>(t.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
}

std::wstring dir_of(const std::wstring& p) {
    auto s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring game_dir() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return dir_of(buf);
}

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void release_tex(Tex& t) {
    if (t.t) t.t->Release();
    t = Tex{};
}

void release_all() {
    for (auto& t : g_bg) release_tex(t);
    for (auto& t : g_beam) release_tex(t);
    release_tex(g_btn_off);
    release_tex(g_btn_on);
    release_tex(g_hero_bg);
    release_tex(g_pointer);
    release_tex(g_white);
    for (auto& kv : g_text) release_tex(kv.second);
    g_text.clear();
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
    g_art_tried = g_art_ok = false;
}

// ---- textures from frontend.big ----------------------------------------------
bool upload(const art::Texture& src, Tex& out) {
    D3DFORMAT fmt = src.pixels == art::PixelFormat::DXT1 ? D3DFMT_DXT1
                  : src.pixels == art::PixelFormat::DXT3 ? D3DFMT_DXT3
                  : src.pixels == art::PixelFormat::DXT5 ? D3DFMT_DXT5 : D3DFMT_A8R8G8B8;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_dev->CreateTexture(src.width, src.height, 1, 0, fmt, D3DPOOL_MANAGED, &t, nullptr))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return false; }
    const bool dxt = fmt != D3DFMT_A8R8G8B8;
    const size_t row_bytes = dxt ? (src.width / 4u) * (fmt == D3DFMT_DXT1 ? 8u : 16u) : src.width * 4u;
    const size_t rows = dxt ? src.height / 4u : src.height;
    for (size_t r = 0; r < rows && (r + 1) * row_bytes <= src.mip0.size(); ++r)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, &src.mip0[r * row_bytes], row_bytes);
    t->UnlockRect(0);
    out.t = t;
    float fw = src.frame_w ? src.frame_w : src.width, fh = src.frame_h ? src.frame_h : src.height;
    out.w = fw;
    out.h = fh;
    out.u1 = fw / src.width;
    out.v1 = fh / src.height;
    return true;
}

bool load(art::Bank& bank, const char* name, Tex& out) {
    art::Texture t;
    if (!bank.load(name, t)) { fo::log("menu: %s", bank.error().c_str()); return false; }
    if (!upload(t, out)) { fo::log("menu: can't create texture %s", name); return false; }
    return true;
}

void make_white() {
    if (SUCCEEDED(g_dev->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_white.t, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_white.t->LockRect(0, &lr, nullptr, 0))) {
            for (int y = 0; y < 4; ++y) for (int x = 0; x < 4; ++x)
                reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xFFFFFFFF;
            g_white.t->UnlockRect(0);
        }
        g_white.w = g_white.h = 4;
    }
}

void load_fonts() {
    if (g_fonts_ok) return;
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(g_self, buf, MAX_PATH);
    std::wstring dir = dir_of(buf) + L"\\fonts\\";
    int n = 0;
    for (const wchar_t* f : {L"Cinzel-SemiBold.ttf", L"Cinzel-Black.ttf", L"CormorantGaramond-SemiBold.ttf"})
        n += AddFontResourceExW((dir + f).c_str(), FR_PRIVATE, nullptr) > 0;
    fo::log("menu: %d of 3 fonts loaded from fonts\\ next to the DLL%s", n, n < 3 ? " (missing ones fall back to Times New Roman)" : "");
    g_fonts_ok = true;
}

void load_art() {
    g_art_tried = true;
    make_white();
    load_fonts();
    std::wstring path = game_dir() + L"\\data\\graphics\\pc\\frontend.big";
    art::Bank bank;
    if (!bank.open(path)) {
        fo::log("menu: can't read data\\graphics\\pc\\frontend.big (%s); menu off", bank.error().c_str());
        return;
    }
    bool ok = true;
    char name[64];
    for (int i = 0; i < 6; ++i) {
        std::snprintf(name, sizeof name, "FRONTEND_BG_0%d_SPRITE", i + 1);
        ok &= load(bank, name, g_bg[i]);
        std::snprintf(name, sizeof name, "FORREST_SUNBEAM_1_%d", i + 1);
        load(bank, name, g_beam[i]);  // optional
    }
    ok &= load(bank, "FE_BUTTON_OFF", g_btn_off);
    ok &= load(bank, "FE_BUTTON_ON", g_btn_on);
    load(bank, "FRONTEND_BACKDROP_01", g_hero_bg);
    load(bank, "MOUSE_POINTER_SPRITE_FE", g_pointer);
    g_art_ok = ok;
    fo::log("menu: Fable front-end art %s", ok ? "loaded" : "incomplete; menu off");
}

// ---- text, baked with GDI into a texture per string -------------------------------
enum class Face { Title, Button, Body, BodyItalic };

// Bakes white text, then colours it top-to-bottom from `top` to `bottom` (a flat colour
// when they match; the title uses a chrome ramp).
Tex& text_tex(const std::string& s, Face face, int px, uint32_t top, uint32_t bottom, const uint32_t* ramp = nullptr, int ramp_n = 0) {
    std::string key = s + '\x1f' + std::to_string(static_cast<int>(face)) + '\x1f' + std::to_string(px) + '\x1f' +
                      std::to_string(top) + '\x1f' + std::to_string(bottom) + '\x1f' + std::to_string(ramp_n);
    auto it = g_text.find(key);
    if (it != g_text.end()) return it->second;
    Tex& out = g_text[key];

    const wchar_t* family = face == Face::Title ? L"Cinzel Black" : face == Face::Button ? L"Cinzel SemiBold" : L"Cormorant Garamond SemiBold";
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_NORMAL, face == Face::BodyItalic, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, VARIABLE_PITCH | FF_ROMAN, family);
    HGDIOBJ old_font = SelectObject(dc, font);
    std::wstring w = utf8_to_wide(s);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, w.c_str(), static_cast<int>(w.size()), &sz);
    int tw = 1, th = 1;
    while (tw < sz.cx + 8) tw *= 2;
    while (th < sz.cy + 8) th *= 2;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tw;
    bi.bmiHeader.biHeight = -th;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    if (bits) std::memset(bits, 0, static_cast<size_t>(tw) * th * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    TextOutW(dc, 4, 4, w.c_str(), static_cast<int>(w.size()));
    GdiFlush();
    if (bits && SUCCEEDED(g_dev->CreateTexture(tw, th, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &out.t, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(out.t->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* src = static_cast<const uint32_t*>(bits);
            for (int y = 0; y < th; ++y) {
                float f = sz.cy > 0 ? std::fmin(1.0f, std::fmax(0.0f, (y - 4.0f) / sz.cy)) : 0;
                uint32_t c;
                if (ramp && ramp_n > 1) {
                    float pos = f * (ramp_n - 1);
                    int i = static_cast<int>(pos);
                    if (i >= ramp_n - 1) i = ramp_n - 2;
                    float k = pos - i;
                    uint32_t a = ramp[i], b = ramp[i + 1];
                    auto mix = [&](int sh) { return static_cast<uint32_t>(((a >> sh) & 0xFF) * (1 - k) + ((b >> sh) & 0xFF) * k) << sh; };
                    c = mix(16) | mix(8) | mix(0);
                } else {
                    auto mix = [&](int sh) { return static_cast<uint32_t>(((top >> sh) & 0xFF) * (1 - f) + ((bottom >> sh) & 0xFF) * f) << sh; };
                    c = mix(16) | mix(8) | mix(0);
                }
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < tw; ++x) {
                    uint32_t px_ = src[y * tw + x];
                    uint32_t cov = ((px_ & 0xFF) + ((px_ >> 8) & 0xFF) + ((px_ >> 16) & 0xFF)) / 3;
                    row[x] = (cov << 24) | c;
                }
            }
            out.t->UnlockRect(0);
        }
    }
    out.w = static_cast<float>(sz.cx);
    out.h = static_cast<float>(sz.cy);
    out.u1 = (sz.cx + 8.0f) / tw;   // text sits at (4, 4) with 4 px of margin round it
    out.v1 = (sz.cy + 8.0f) / th;
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    return out;
}

// ---- drawing ---------------------------------------------------------------------
void flush() {
    if (g_verts.empty()) return;
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(g_verts.size() / 3), g_verts.data(), sizeof(Vertex));
    g_verts.clear();
}

void bind(IDirect3DTexture9* t) {
    if (t == g_bound) return;
    flush();
    g_dev->SetTexture(0, t);
    g_bound = t;
}

void additive(bool on) {
    flush();
    g_dev->SetRenderState(D3DRS_DESTBLEND, on ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
}

void quad(const Tex& t, float x0, float y0, float x1, float y1, uint32_t c, float u0 = 0, float v0 = 0, float u1 = -1, float v1 = -1) {
    if (!t.t) return;
    if (u1 < 0) u1 = t.u1;
    if (v1 < 0) v1 = t.v1;
    bind(t.t);
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    Vertex a{x0, y0, 0, 1, c, u0, v0}, b{x1, y0, 0, 1, c, u1, v0}, d{x0, y1, 0, 1, c, u0, v1}, e{x1, y1, 0, 1, c, u1, v1};
    g_verts.insert(g_verts.end(), {a, b, d, b, e, d});
}

void rect(float x, float y, float w, float h, uint32_t c) { quad(g_white, x, y, x + w, y + h, c); }

// Draws text with its top-left at (x, y); align 0 = left, 1 = centre. Returns its width.
float text(const std::string& s, Face face, float design_px, float x, float y, uint32_t color, int align = 0, bool shadow = true) {
    int px = static_cast<int>(std::lround(design_px * g_model.scale()));
    if (px < 6) px = 6;
    Tex& t = text_tex(s, face, px, 0xFFFFFFFF, 0xFFFFFFFF);
    if (align == 1) x -= t.w / 2;
    const float pad = 4;
    // The texture holds the text at (pad, pad) and u1/v1 cover text + pad on each side.
    float qw = t.w + 2 * pad, qh = t.h + 2 * pad;
    if (shadow) quad(t, x - pad + 1.5f, y - pad + 2, x - pad + 1.5f + qw, y - pad + 2 + qh, color & 0xC0000000);
    quad(t, x - pad, y - pad, x - pad + qw, y - pad + qh, color);
    return t.w;
}

void draw_backdrop(const Tex* tiles, uint32_t color, float sw, float sh) {
    // 640x480 picture from six tiles: 256 + 256 + 128 wide, 256 + 224 tall (the frame).
    const float W = 640, H = 480;
    float s = std::fmax(sw / W, sh / H);
    float ox = (sw - W * s) / 2, oy = (sh - H * s) / 2;
    const float xs[4] = {0, 256, 512, 640}, ys[3] = {0, 256, 480};
    for (int r = 0; r < 2; ++r)
        for (int c = 0; c < 3; ++c) {
            const Tex& t = tiles[r * 3 + c];
            if (!t.t) continue;
            float tw = xs[c + 1] - xs[c], th = ys[r + 1] - ys[r];
            float v1 = th / 256.0f;  // tiles are 256 tall; the bottom row uses 224 of them
            // Overlap by a pixel to hide seams from filtering.
            quad(t, ox + xs[c] * s, oy + ys[r] * s, ox + (xs[c] + tw) * s + 1, oy + (ys[r] + th) * s + 1, color, 0, 0, 1, v1 * 1.0f);
        }
}

void draw_single_backdrop(const Tex& t, float sw, float sh, float shift) {
    if (!t.t) return;
    float s = std::fmax(sw / t.w, sh / t.h);
    float w = t.w * s, h = t.h * s;
    quad(t, (sw - w) / 2 + shift * sw, (sh - h) / 2, (sw - w) / 2 + shift * sw + w, (sh - h) / 2 + h, 0xFFFFFFFF);
}

void vignette(float sw, float sh) {
    // Darken the edges in bands (no shader needed).
    const int n = 12;
    for (int i = 0; i < n; ++i) {
        float f = static_cast<float>(i) / n;
        uint32_t a = static_cast<uint32_t>(150 * (1 - f) * (1 - f) / n * 3);
        if (a > 255) a = 255;
        float inset_x = f * sw * 0.22f, inset_y = f * sh * 0.22f;
        rect(0, 0, inset_x + sw * 0.02f, sh, a << 24);
        rect(sw - inset_x - sw * 0.02f, 0, inset_x + sw * 0.02f, sh, a << 24);
        rect(0, 0, sw, inset_y + sh * 0.02f, a << 24);
        rect(0, sh - inset_y - sh * 0.02f, sw, inset_y + sh * 0.02f, a << 24);
    }
}

void draw_button(const Button& b) {
    const Tex& art = b.lit ? g_btn_on : g_btn_off;
    quad(art, b.x, b.y, b.x + b.w, b.y + b.h, 0xFFFFFFFF);
    float size = b.h > 70 * g_model.scale() ? 19.0f : 17.0f;
    float ty = b.y + b.h / 2 - size * g_model.scale() * 0.68f;
    text(b.label, Face::Button, size, b.x + b.w / 2, ty, b.lit ? kTextLit : kText, 1);
}

void draw_title(float sw) {
    const float s = g_model.scale();
    static const uint32_t chrome[] = {0xFFF4F7FF, 0xFFC9D3FF, 0xFF8F9CF0, 0xFF3D4A9A, 0xFF9FB0F6, 0xFFE9EEFF};
    int px = static_cast<int>(std::lround(64 * s));
    Tex& t = text_tex("CHAPTERS OF THE LOST", Face::Title, px, 0, 0, chrome, 6);
    float x = sw / 2 - t.w / 2, y = g_model.sy(56);
    float qw = t.w + 8, qh = t.h + 8;
    quad(t, x - 4 + 0, y - 4 + 3 * s, x - 4 + qw, y - 4 + 3 * s + qh, 0xD0000000);   // dark drop
    quad(t, x - 4, y - 4, x - 4 + qw, y - 4 + qh, 0xFFFFFFFF);
    text("O N L I N E", Face::Button, 26, sw / 2, y + t.h + 2 * s, 0xFFF1ECE0, 1);
}

void draw_status(float sw, float sh) {
    if (g_status.empty()) return;
    double age = now_s() - g_status_since;
    if (age > 4.0) return;
    uint32_t a = age > 3.0 ? static_cast<uint32_t>(255 * (4.0 - age)) : 255;
    text(g_status, Face::BodyItalic, 20, sw / 2, sh - 70 * g_model.scale(), (a << 24) | (kMuted & 0xFFFFFF), 1);
}

void draw_title_screen(float sw, float sh) {
    double t = now_s() - g_t0;
    draw_backdrop(g_bg, 0xFFFFFFFF, sw, sh);
    additive(true);
    uint32_t beam = static_cast<uint32_t>(90 + 80 * (0.5 + 0.5 * std::sin(t * 0.9)));
    draw_backdrop(g_beam, (beam << 24) | 0xFFFFFF, sw, sh);
    additive(false);
    vignette(sw, sh);
    draw_title(sw);
    for (const auto& b : g_model.buttons()) draw_button(b);
    const float s = g_model.scale();
    if (!g_model.realm_line().empty())
        text(g_model.realm_line(), Face::Body, 17, 40 * s, sh - 52 * s, kMuted);
}

void draw_heroes_screen(float sw, float sh) {
    const float s = g_model.scale();
    rect(0, 0, sw, sh, 0xFF000000);
    draw_single_backdrop(g_hero_bg, sw, sh, 0.12f);
    // Darken the left side for the list.
    for (int i = 0; i < 16; ++i) {
        float f = i / 16.0f;
        uint32_t a = static_cast<uint32_t>(235 * (1 - f));
        rect(sw * 0.6f * f, 0, sw * 0.6f / 16 + 1, sh, a << 24);
    }
    text("CHOOSE YOUR HERO", Face::Button, 34, g_model.sx(56), g_model.sy(44), 0xFFF1ECE0);
    for (const auto& b : g_model.buttons()) draw_button(b);
    text("Each new hero lives their childhood and Guild", Face::BodyItalic, 18, g_model.sx(74), g_model.sy(290), kMuted);
    text("training alone, then joins the shared world.", Face::BodyItalic, 18, g_model.sx(74), g_model.sy(312), kMuted);

    Rect p = g_model.details_panel();
    rect(p.x, p.y, p.w, p.h, 0x9E080806);
    rect(p.x, p.y, p.w, 1, 0x80C9C0A8);
    rect(p.x, p.y + p.h - 1, p.w, 1, 0x80C9C0A8);
    float x = p.x + 22 * s, y = p.y + 18 * s;
    text(g_model.hero_name(), Face::Button, 22, x, y, 0xFFF1ECE0);
    text("RENOWN, STATS AND GOLD", Face::Button, 12, x, y + 44 * s, kLabel, 0, false);
    text("are kept by the game server", Face::Body, 18, x, y + 62 * s, kText);
    text("and appear here once it is live.", Face::Body, 18, x, y + 84 * s, kText);
    text("LAST SEEN", Face::Button, 12, x, y + 124 * s, kLabel, 0, false);
    text("Your latest save", Face::Body, 18, x, y + 142 * s, kText);
}

void begin_states(float sw, float sh) {
    D3DVIEWPORT9 vp = {0, 0, static_cast<DWORD>(sw), static_cast<DWORD>(sh), 0.0f, 1.0f};
    g_dev->SetViewport(&vp);
    g_dev->SetVertexShader(nullptr);
    g_dev->SetPixelShader(nullptr);
    g_dev->SetFVF(kFvf);
    for (DWORD i = 1; i < 8; ++i) { g_dev->SetTexture(i, nullptr); g_dev->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE); }
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    g_dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    g_dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    g_dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g_dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    g_bound = nullptr;
}

void handle(Action a) {   // caller holds g_mu
    switch (a) {
    case Action::EnterAlbion:
        g_active = false;
        fo::log("menu: Enter Albion; handing over to Fable's own menu (pick Continue or Load to play)");
        break;
    case Action::Options:
        g_active = false;
        fo::log("menu: Options; showing Fable's own options menu");
        break;
    case Action::ModSettings:
        fo::log("menu: Mod settings");
        modsettings::open(modsettings::kExtras);
        break;
    case Action::Quit:
        fo::log("menu: Leave Albion; closing the game");
        if (HWND w = GetForegroundWindow()) PostMessageW(w, WM_CLOSE, 0, 0);
        else ExitProcess(0);
        break;
    default:
        break;
    }
}
}  // namespace

void set_module(HMODULE self) { g_self = self; }

bool active() {
    if (!g_active.load() || g_disabled.load()) return false;
    return config().menu_enabled;
}

void draw(IDirect3DDevice9* dev) {
    if (!active()) return;
    if (dev != g_dev) { release_all(); g_dev = dev; g_t0 = now_s(); }
    if (!g_art_tried) load_art();
    if (!g_art_ok) { g_disabled = true; return; }   // never leave the player behind a blank menu

    IDirect3DSurface9* rt = nullptr;
    float sw = 0, sh = 0;
    if (SUCCEEDED(g_dev->GetRenderTarget(0, &rt))) {
        D3DSURFACE_DESC d;
        rt->GetDesc(&d);
        rt->Release();
        sw = static_cast<float>(d.Width);
        sh = static_cast<float>(d.Height);
    }
    if (sw <= 0 || sh <= 0) return;
    if (!g_saved && FAILED(g_dev->CreateStateBlock(D3DSBT_ALL, &g_saved))) return;
    g_saved->Capture();
    begin_states(sw, sh);

    std::lock_guard<std::mutex> lock(g_mu);
    g_model.resize(sw, sh);
    if (g_model.scale() != g_text_scale) {     // text is baked at screen size: rebake on resize
        for (auto& kv : g_text) release_tex(kv.second);
        g_text.clear();
        g_text_scale = g_model.scale();
    }
    if (g_model.status_serial() != g_status_serial) {
        g_status_serial = g_model.status_serial();
        g_status = g_model.status();
        g_status_since = now_s();
    }
    if (g_px < 0) {
        g_px = sw / 2;
        g_py = sh * 0.6f;
        const Config& cfg = config();
        std::string name = cfg.name;
        if (name.empty()) {
            char user[128] = {};
            DWORD n = sizeof user;
            name = GetUserNameA(user, &n) ? user : "Hero";
        }
        g_model.set_hero_name(name);
        g_model.set_realm_line("Server " + cfg.chat_host + ":" + std::to_string(cfg.chat_port));
    }

    if (g_model.screen() == Screen::Title) draw_title_screen(sw, sh);
    else draw_heroes_screen(sw, sh);
    draw_status(sw, sh);
    if (g_pointer.t) quad(g_pointer, g_px, g_py, g_px + 32 * g_model.scale(), g_py + 32 * g_model.scale(), 0xFFFFFFFF);
    flush();
    g_saved->Apply();
    ++g_frames;
}

void on_device_lost() {
    if (g_saved) { g_saved->Release(); g_saved = nullptr; }
}

bool on_message(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (!active()) return false;
    std::lock_guard<std::mutex> lock(g_mu);
    auto to_bb = [&](LPARAM l) {
        RECT rc;
        GetClientRect(w, &rc);
        float cw = static_cast<float>(rc.right - rc.left), ch = static_cast<float>(rc.bottom - rc.top);
        // The model holds the back-buffer size (set every frame); the window may be scaled.
        float bw = g_model.screen_w(), bh = g_model.screen_h();
        g_client_to_bb_x = cw > 0 ? bw / cw : 1;
        g_client_to_bb_y = ch > 0 ? bh / ch : 1;
        g_px = static_cast<float>(static_cast<short>(LOWORD(l))) * g_client_to_bb_x;
        g_py = static_cast<float>(static_cast<short>(HIWORD(l))) * g_client_to_bb_y;
    };
    switch (msg) {
    case WM_MOUSEMOVE:
        to_bb(lp);
        g_last_wm_mouse = GetTickCount();
        g_model.pointer(g_px, g_py);
        return true;
    case WM_LBUTTONDOWN:
        to_bb(lp);
        g_last_wm_mouse = GetTickCount();
        handle(g_model.click(g_px, g_py));
        return true;
    case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_MOUSEWHEEL: case WM_LBUTTONDBLCLK:
        return true;
    case WM_KEYDOWN: {
        Key k;
        switch (wp) {
        case VK_UP: k = Key::Up; break;
        case VK_DOWN: k = Key::Down; break;
        case VK_LEFT: k = Key::Left; break;
        case VK_RIGHT: k = Key::Right; break;
        case VK_RETURN: case VK_SPACE: k = Key::Enter; break;
        case VK_ESCAPE: k = Key::Escape; break;
        default: return true;
        }
        handle(g_model.key(k));
        return true;
    }
    case WM_KEYUP: case WM_CHAR: case WM_SYSKEYDOWN:
        return msg != WM_SYSKEYDOWN || wp != VK_F4;   // keep Alt+F4
    }
    return false;
}

void on_mouse_state(const void* state, DWORD size) {
    if (!active() || !state || size < 16) return;
    LONG d[3];
    std::memcpy(d, state, sizeof d);
    bool button = (static_cast<const uint8_t*>(state)[12] & 0x80) != 0;
    std::lock_guard<std::mutex> lock(g_mu);
    if (GetTickCount() - g_last_wm_mouse > 500) {   // only when the window gets no mouse messages
        float s = g_model.scale() > 0 ? g_model.scale() : 1;
        g_px = std::fmin(std::fmax(g_px + d[0] * s, 0.0f), g_model.screen_w());
        g_py = std::fmin(std::fmax(g_py + d[1] * s, 0.0f), g_model.screen_h());
        g_model.pointer(g_px, g_py);
        if (button && !g_dinput_button) handle(g_model.click(g_px, g_py));
    }
    g_dinput_button = button;
}

void on_mouse_data(const void* objects, DWORD object_size, DWORD count) {
    if (!active() || !objects || object_size < 8) return;
    // DIDEVICEOBJECTDATA: DWORD dwOfs, DWORD dwData, ... ; offsets 0 = X, 4 = Y, 12 = button 0.
    LONG state[4] = {0, 0, 0, 0};
    for (DWORD i = 0; i < count; ++i) {
        const uint8_t* o = static_cast<const uint8_t*>(objects) + i * object_size;
        DWORD ofs, data;
        std::memcpy(&ofs, o, 4);
        std::memcpy(&data, o + 4, 4);
        if (ofs == 0) state[0] += static_cast<LONG>(data);
        else if (ofs == 4) state[1] += static_cast<LONG>(data);
        else if (ofs == 12) state[3] = (data & 0x80) ? 0x80 : 0;
    }
    uint8_t buf[16];
    std::memcpy(buf, state, 12);
    buf[12] = static_cast<uint8_t>(state[3] ? 0x80 : (g_dinput_button ? 0x80 : 0));
    buf[13] = buf[14] = buf[15] = 0;
    on_mouse_state(buf, sizeof buf);
}

void log_status() {
    fo::log("menu: %s, %u frames drawn, art %s", active() ? "showing" : "hidden", g_frames,
            g_art_ok ? "ok" : (g_art_tried ? "missing" : "not loaded yet"));
}

}  // namespace fo::menu
