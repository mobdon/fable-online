#include "renderer.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include "../log.h"
#include "../ui/chat_model.h"

namespace fo::overlay {
namespace {
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

// Builds a Font in the same layout as Fable's, from a Windows font, for when fonts.big is missing.
void gdi_font(const wchar_t* face, int px, int weight, assets::Font& out) {
    out = assets::Font{};
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    HGDIOBJ old_font = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    const int cw = tm.tmMaxCharWidth + 2, ch = tm.tmHeight + 2, cols = 16;
    out.face = "gdi";
    out.height = tm.tmHeight;
    out.line = tm.tmHeight;
    out.tex_w = 128;
    while (out.tex_w < cw * cols) out.tex_w *= 2;
    out.tex_h = 128;
    while (out.tex_h < ch * 14) out.tex_h *= 2;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = out.tex_w;
    bi.bmiHeader.biHeight = -out.tex_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(out.tex_w) * out.tex_h * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (int c = 32; c < 256; ++c) {
        int cell = c - 32, x = (cell % cols) * cw, y = (cell / cols) * ch;
        wchar_t w = static_cast<wchar_t>(c);
        SIZE sz;
        GetTextExtentPoint32W(dc, &w, 1, &sz);
        TextOutW(dc, x + 1, y + 1, &w, 1);
        assets::Glyph& g = out.glyphs[c];
        g = {true, x, y, x + cw, y + ch, -1, static_cast<int>(sz.cx), static_cast<int>(sz.cx)};
    }
    GdiFlush();
    const uint32_t* src = static_cast<const uint32_t*>(bits);
    out.argb.resize(static_cast<size_t>(out.tex_w) * out.tex_h);
    for (size_t i = 0; i < out.argb.size(); ++i) out.argb[i] = ((src[i] & 0xFF) << 24) | 0x00FFFFFF;
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
}

// Shelf packing into the atlas.
struct Packer {
    int w, h, x = 0, y = 0, row = 0;
    bool place(int iw, int ih, int& ox, int& oy) {
        if (x + iw > w) { x = 0; y += row + 1; row = 0; }
        if (y + ih > h || iw > w) return false;
        ox = x; oy = y;
        x += iw + 1;
        row = std::max(row, ih);
        return true;
    }
};

uint32_t scale_alpha(uint32_t argb, float a) {
    uint32_t na = static_cast<uint32_t>(((argb >> 24) & 0xFF) * std::clamp(a, 0.0f, 1.0f) + 0.5f);
    return (na << 24) | (argb & 0x00FFFFFF);
}
}  // namespace

bool Renderer::build_atlas(const assets::UiArt* art, int fallback_px) {
    fable_fonts_ = art && art->fonts_ok;
    fable_frame_ = art && art->frame_ok;
    const assets::Font* fonts[3];
    if (fable_fonts_) {
        fonts[0] = &art->chat; fonts[1] = &art->small; fonts[2] = &art->title;
    } else {
        gdi_font(L"Tahoma", fallback_px, FW_NORMAL, fallback_[0]);
        gdi_font(L"Tahoma", fallback_px - 3, FW_NORMAL, fallback_[1]);
        gdi_font(L"Arial", fallback_px + 5, FW_NORMAL, fallback_[2]);
        fonts[0] = &fallback_[0]; fonts[1] = &fallback_[1]; fonts[2] = &fallback_[2];
    }
    if (art) fill_ = art->fill;

    std::vector<uint32_t> px(static_cast<size_t>(tex_w_) * tex_h_, 0);
    Packer pk{tex_w_, tex_h_};
    auto blit = [&](const uint32_t* src, int w, int h, Rect& at) {
        if (!pk.place(w, h, at.x, at.y)) return false;
        at.w = w; at.h = h;
        for (int y = 0; y < h; ++y) std::memcpy(&px[static_cast<size_t>(at.y + y) * tex_w_ + at.x], src + static_cast<size_t>(y) * w, w * 4);
        return true;
    };
    for (int i = 0; i < 3; ++i) {
        faces_[i].font = fonts[i];
        if (!blit(fonts[i]->argb.data(), fonts[i]->tex_w, fonts[i]->tex_h, faces_[i].at)) return false;
    }
    uint32_t white[16];
    std::fill(std::begin(white), std::end(white), 0xFFFFFFFFu);
    if (!blit(white, 4, 4, white_)) return false;
    fable_bar_ = art && art->bar_ok;
    for (int on = 0; on < 2 && fable_bar_; ++on)
        for (int i = 0; i < 3; ++i) {
            const art::Image& im = on ? art->bar_on[i] : art->bar_off[i];
            if (!blit(im.px.data(), im.w, im.h, bar_[on][i])) { fable_bar_ = false; break; }
        }
    if (fable_frame_) {
        const art::Image* imgs[] = {&art->tl, &art->tr, &art->bl, &art->br, &art->top, &art->bottom, &art->left, &art->right};
        Rect* slots[] = {&tl_, &tr_, &bl_, &br_, &top_, &bottom_, &left_, &right_};
        for (int i = 0; i < 8; ++i)
            if (!blit(imgs[i]->px.data(), imgs[i]->w, imgs[i]->h, *slots[i])) { fable_frame_ = false; break; }
    }

    fable_msn_ = art && art->msn_ok;
    for (int i = 0; i < 9 && fable_msn_; ++i)
        if (!blit(art->msn[i].px.data(), art->msn[i].w, art->msn[i].h, msn_[i])) fable_msn_ = false;

    if (FAILED(dev_->CreateTexture(tex_w_, tex_h_, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex_, nullptr))) return false;
    D3DLOCKED_RECT lr;
    if (FAILED(tex_->LockRect(0, &lr, nullptr, 0))) { tex_->Release(); tex_ = nullptr; return false; }
    for (int y = 0; y < tex_h_; ++y)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &px[static_cast<size_t>(y) * tex_w_], tex_w_ * 4);
    tex_->UnlockRect(0);
    fo::log("overlay: text %s, frame %s, chat window %s", fable_fonts_ ? "Fable's own fonts" : "Windows Tahoma (fonts.big not read)",
            fable_frame_ ? "Fable's pewter frame" : "plain", fable_msn_ ? "the PC chat screen frame" : "the pewter frame");
    return true;
}

bool Renderer::begin(IDirect3DDevice9* dev, const assets::UiArt* art, int fallback_px) {
    if (dev != dev_) { release(); dev_ = dev; }
    if (!tex_ && !build_atlas(art, fallback_px)) return false;
    if (!saved_ && FAILED(dev_->CreateStateBlock(D3DSBT_ALL, &saved_))) return false;
    saved_->Capture();

    D3DVIEWPORT9 vp;
    dev_->GetViewport(&vp);
    IDirect3DSurface9* rt = nullptr;
    if (SUCCEEDED(dev_->GetRenderTarget(0, &rt))) {
        D3DSURFACE_DESC d;
        rt->GetDesc(&d);
        rt->Release();
        vp = {0, 0, d.Width, d.Height, 0.0f, 1.0f};
        dev_->SetViewport(&vp);
    }
    screen_w_ = static_cast<float>(vp.Width);
    screen_h_ = static_cast<float>(vp.Height);

    dev_->SetVertexShader(nullptr);
    dev_->SetPixelShader(nullptr);
    dev_->SetFVF(kFvf);
    dev_->SetTexture(0, tex_);
    for (DWORD i = 1; i < 8; ++i) { dev_->SetTexture(i, nullptr); dev_->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE); }
    dev_->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev_->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev_->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev_->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev_->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev_->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev_->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev_->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev_->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev_->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev_->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev_->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev_->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev_->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev_->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev_->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev_->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    dev_->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev_->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev_->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev_->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev_->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev_->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
    verts_.clear();
    return true;
}

void Renderer::flush() {
    if (verts_.empty()) return;
    dev_->DrawPrimitiveUP(D3DPT_TRIANGLELIST, static_cast<UINT>(verts_.size() / 3), verts_.data(), sizeof(Vertex));
    verts_.clear();
}

void Renderer::end() {
    flush();
    if (saved_) saved_->Apply();
}

void Renderer::on_lost() {
    if (saved_) { saved_->Release(); saved_ = nullptr; }
}

void Renderer::release() {
    on_lost();
    if (tex_) { tex_->Release(); tex_ = nullptr; }
    dev_ = nullptr;
}

void Renderer::quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;  // texel centres onto pixel centres (D3D9)
    Vertex a{x0, y0, 0, 1, c, u0, v0}, b{x1, y0, 0, 1, c, u1, v0}, d{x0, y1, 0, 1, c, u0, v1}, e{x1, y1, 0, 1, c, u1, v1};
    verts_.insert(verts_.end(), {a, b, d, b, e, d});
    if (verts_.size() > 6000) flush();
}

void Renderer::sprite(const Rect& r, float x, float y, float w, float h, uint32_t c) {
    quad(x, y, x + w, y + h, static_cast<float>(r.x) / tex_w_, static_cast<float>(r.y) / tex_h_,
         static_cast<float>(r.x + r.w) / tex_w_, static_cast<float>(r.y + r.h) / tex_h_, c);
}

void Renderer::rect(float x, float y, float w, float h, uint32_t argb) {
    float u = (white_.x + 2.0f) / tex_w_, v = (white_.y + 2.0f) / tex_h_;
    quad(x, y, x + w, y + h, u, v, u, v, argb);
}

void Renderer::panel(float x, float y, float w, float h, float a) {
    rect(x + 3, y + 3, w - 6, h - 6, scale_alpha(fill_, 0.82f * a));
    uint32_t c = scale_alpha(0xFFFFFFFF, a);
    if (!fable_frame_) {
        uint32_t pewter = scale_alpha(0xFF938D7E, a);
        rect(x, y, w, 2, pewter); rect(x, y + h - 2, w, 2, pewter);
        rect(x, y, 2, h, pewter); rect(x + w - 2, y, 2, h, pewter);
        return;
    }
    // Fable's table frame: 32px corners (drawn at 24px so the rivets don't crowd the text)
    // and 8px edge strips stretched between them.
    const float cs = 24, e = 6;
    sprite(tl_, x, y, cs, cs, c);
    sprite(tr_, x + w - cs, y, cs, cs, c);
    sprite(bl_, x, y + h - cs, cs, cs, c);
    sprite(br_, x + w - cs, y + h - cs, cs, cs, c);
    sprite(top_, x + cs, y, w - 2 * cs, e, c);
    sprite(bottom_, x + cs, y + h - e, w - 2 * cs, e, c);
    sprite(left_, x, y + cs, e, h - 2 * cs, c);
    sprite(right_, x + w - e, y + cs, e, h - 2 * cs, c);
}

void Renderer::window(float x, float y, float w, float h, float a) {
    if (!fable_msn_) { panel(x, y, w, h, a); return; }
    rect(x + 4, y + 4, w - 8, h - 8, scale_alpha(fill_, 0.82f * a));
    uint32_t c = scale_alpha(0xFFFFFFFF, a);
    // Corners and edges at their own size (capped, so a big frame can't swallow a small
    // window); edges and centre stretch.
    auto cap = [](int v) { return static_cast<float>(v > 40 ? 40 : v); };
    const float l = cap(msn_[0].w), r = cap(msn_[2].w), t = cap(msn_[0].h), b = cap(msn_[6].h);
    const float mw = w - l - r, mh = h - t - b;
    sprite(msn_[4], x + l, y + t, mw, mh, c);
    sprite(msn_[0], x, y, l, t, c);
    sprite(msn_[1], x + l, y, mw, t, c);
    sprite(msn_[2], x + w - r, y, r, t, c);
    sprite(msn_[3], x, y + t, l, mh, c);
    sprite(msn_[5], x + w - r, y + t, r, mh, c);
    sprite(msn_[6], x, y + h - b, l, b, c);
    sprite(msn_[7], x + l, y + h - b, mw, b, c);
    sprite(msn_[8], x + w - r, y + h - b, r, b, c);
}

float Renderer::bar(float x, float y, float w, float h, float a, bool on, bool ends) {
    if (!fable_bar_) {
        rect(x, y, w, h, scale_alpha(on ? 0xC0A28F74 : 0xA0938D7E, a));
        return 0;
    }
    uint32_t c = scale_alpha(0xFFFFFFFF, a);
    const Rect* b = bar_[on ? 1 : 0];
    if (!ends) { sprite(b[1], x, y, w, h, c); return 0; }
    float e = b[0].w * h / b[0].h;          // keep the ornate ends' proportions
    if (w < 2 * e) w = 2 * e;
    sprite(b[0], x, y, e, h, c);
    sprite(b[1], x + e, y, w - 2 * e, h, c);
    sprite(b[2], x + w - e, y, e, h, c);
    return e;
}

void Renderer::rule(float x, float y, float w, float a) {
    if (fable_frame_) sprite(top_, x, y, w, 6, scale_alpha(0xFFFFFFFF, a));
    else rect(x, y + 2, w, 2, scale_alpha(0xFF938D7E, a));
}

float Renderer::line_height(Face f) const {
    const assets::Font* font = faces_[static_cast<int>(f)].font;
    return font ? static_cast<float>(font->line) : 16.0f;
}

const assets::Glyph& Renderer::glyph(Face f, char32_t c) const {
    const assets::Font& font = *faces_[static_cast<int>(f)].font;
    if (c < 256 && font.glyphs[c].valid) return font.glyphs[c];
    if (c == 0xB7 && font.glyphs['-'].valid) return font.glyphs['-'];  // Fable's fonts stop at '~'
    return font.glyphs['?'];
}

float Renderer::measure(const std::string& s, Face f) const {
    if (!faces_[static_cast<int>(f)].font) return 0;
    float w = 0;
    for (char32_t c : ui::utf8_to_u32(s)) w += static_cast<float>(glyph(f, c).advance);
    return w;
}

float Renderer::text(float x, float y, const std::string& s, uint32_t argb, Face f, bool outline) {
    const FaceInfo& fi = faces_[static_cast<int>(f)];
    if (!fi.font) return 0;
    std::u32string u = ui::utf8_to_u32(s);
    x = static_cast<float>(static_cast<int>(x + 0.5f));  // whole pixels keep bitmap glyphs crisp
    y = static_cast<float>(static_cast<int>(y + 0.5f));
    const uint32_t a = argb >> 24;
    const uint32_t shadow = ((a * 3 / 4) << 24);
    static const float kOutline[][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {1, 1}};
    static const float kShadow[][2] = {{1, 1}};
    const float (*offs)[2] = outline ? kOutline : kShadow;
    const int passes = outline ? 5 : 1;
    float pen = x;
    for (int pass = 0; pass <= passes; ++pass) {
        float dx = pass < passes ? offs[pass][0] : 0, dy = pass < passes ? offs[pass][1] : 0;
        uint32_t col = pass < passes ? shadow : argb;
        pen = x;
        for (char32_t c : u) {
            const assets::Glyph& g = glyph(f, c);
            if (c != U' ' && g.x1 > g.x0) {
                float gx = pen + g.bearing + dx, gy = y + dy;
                quad(gx, gy, gx + (g.x1 - g.x0), gy + (g.y1 - g.y0),
                     static_cast<float>(fi.at.x + g.x0) / tex_w_, static_cast<float>(fi.at.y + g.y0) / tex_h_,
                     static_cast<float>(fi.at.x + g.x1) / tex_w_, static_cast<float>(fi.at.y + g.y1) / tex_h_, col);
            }
            pen += static_cast<float>(g.advance);
        }
    }
    return pen - x;
}

std::vector<std::string> Renderer::wrap(const std::string& s, float max_w, Face f) const {
    std::vector<std::string> out;
    std::u32string u = ui::utf8_to_u32(s), line;
    float w = 0;
    size_t last_space = std::u32string::npos;
    for (char32_t c : u) {
        float cw = static_cast<float>(glyph(f, c).advance);
        if (w + cw > max_w && !line.empty()) {
            if (last_space != std::u32string::npos && last_space > 0) {
                out.push_back(ui::u32_to_utf8(line.substr(0, last_space)));
                line = line.substr(last_space + 1);
            } else {
                out.push_back(ui::u32_to_utf8(line));
                line.clear();
            }
            w = 0;
            last_space = std::u32string::npos;
            for (size_t i = 0; i < line.size(); ++i) {
                w += static_cast<float>(glyph(f, line[i]).advance);
                if (line[i] == U' ') last_space = i;
            }
        }
        if (c == U' ') last_space = line.size();
        line += c;
        w += cw;
    }
    if (!line.empty() || out.empty()) out.push_back(ui::u32_to_utf8(line));
    for (size_t i = 1; i < out.size(); ++i) out[i] = "  " + out[i];  // indent continuation lines
    return out;
}

}  // namespace fo::overlay
