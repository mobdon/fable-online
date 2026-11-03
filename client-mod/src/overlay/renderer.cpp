#include "renderer.h"
#include <windows.h>
#include <cstring>
#include "../log.h"
#include "../ui/chat_model.h"

namespace fo::overlay {
namespace {
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;
constexpr int kCols = 16;   // 16 x 14 cells hold characters 32..255

unsigned char glyph_of(char32_t c) { return (c >= 32 && c <= 255 && !(c >= 0x7F && c < 0xA0)) ? static_cast<unsigned char>(c) : '?'; }
}  // namespace

bool Renderer::build_font(int px) {
    HDC dc = CreateCompatibleDC(nullptr);
    HFONT font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Tahoma");
    HGDIOBJ old_font = SelectObject(dc, font);
    TEXTMETRICW tm;
    GetTextMetricsW(dc, &tm);
    cell_h_ = tm.tmHeight + 2;
    cell_w_ = tm.tmMaxCharWidth + 2;
    tex_w_ = 512;
    tex_h_ = 512;
    while (cell_w_ * kCols > tex_w_ && tex_w_ < 2048) tex_w_ *= 2;
    while (cell_h_ * 15 > tex_h_ && tex_h_ < 2048) tex_h_ *= 2;

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = tex_w_;
    bi.bmiHeader.biHeight = -tex_h_;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old_bmp = SelectObject(dc, bmp);
    std::memset(bits, 0, static_cast<size_t>(tex_w_) * tex_h_ * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    for (int c = 32; c < 256; ++c) {
        int cell = c - 32, cx = (cell % kCols) * cell_w_, cy = (cell / kCols) * cell_h_;
        wchar_t w = static_cast<wchar_t>(c);
        SIZE sz;
        GetTextExtentPoint32W(dc, &w, 1, &sz);
        adv_[c] = static_cast<float>(sz.cx);
        TextOutW(dc, cx + 1, cy + 1, &w, 1);
    }
    GdiFlush();

    bool ok = false;
    if (SUCCEEDED(dev_->CreateTexture(tex_w_, tex_h_, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex_, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(tex_->LockRect(0, &lr, nullptr, 0))) {
            const uint32_t* src = static_cast<const uint32_t*>(bits);
            for (int y = 0; y < tex_h_; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (int x = 0; x < tex_w_; ++x) {
                    uint32_t a = src[y * tex_w_ + x] & 0xFF;  // white on black: any channel is coverage
                    row[x] = (a << 24) | 0x00FFFFFF;
                }
            }
            // A solid white block in the last row of cells, for rect().
            for (int y = tex_h_ - 4; y < tex_h_; ++y) {
                uint32_t* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (int x = tex_w_ - 4; x < tex_w_; ++x) row[x] = 0xFFFFFFFF;
            }
            tex_->UnlockRect(0);
            ok = true;
        }
    }
    SelectObject(dc, old_bmp);
    SelectObject(dc, old_font);
    DeleteObject(bmp);
    DeleteObject(font);
    DeleteDC(dc);
    font_px_ = px;
    fo::log("overlay: font %dpx, atlas %dx%d, %s", px, tex_w_, tex_h_, ok ? "ok" : "FAILED");
    return ok;
}

bool Renderer::begin(IDirect3DDevice9* dev, int font_px) {
    if (dev != dev_) { release(); dev_ = dev; }
    if (!tex_ && !build_font(font_px)) return false;
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
    // -0.5 maps texels to pixels exactly in D3D9.
    x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
    Vertex a{x0, y0, 0, 1, c, u0, v0}, b{x1, y0, 0, 1, c, u1, v0}, d{x0, y1, 0, 1, c, u0, v1}, e{x1, y1, 0, 1, c, u1, v1};
    verts_.insert(verts_.end(), {a, b, d, b, e, d});
    if (verts_.size() > 6000) flush();
}

void Renderer::rect(float x, float y, float w, float h, uint32_t argb) {
    float u = (tex_w_ - 2.0f) / tex_w_, v = (tex_h_ - 2.0f) / tex_h_;
    quad(x, y, x + w, y + h, u, v, u, v, argb);
}

float Renderer::measure(const std::string& s) const {
    float w = 0;
    for (char32_t c : ui::utf8_to_u32(s)) w += adv_[glyph_of(c)];
    return w;
}

float Renderer::text(float x, float y, const std::string& s, uint32_t argb, bool shadow) {
    std::u32string u = ui::utf8_to_u32(s);
    float pen = x;
    uint32_t shade = (argb & 0xFF000000) ? ((((argb >> 24) * 3 / 4) << 24)) : 0;
    for (int pass = shadow ? 0 : 1; pass < 2; ++pass) {
        pen = x;
        for (char32_t c : u) {
            unsigned char g = glyph_of(c);
            int cell = g - 32;
            float cx = static_cast<float>((cell % kCols) * cell_w_), cy = static_cast<float>((cell / kCols) * cell_h_);
            float off = pass == 0 ? 1.0f : 0.0f;
            if (g != ' ')
                quad(pen + off - 1, y + off - 1, pen + off - 1 + cell_w_, y + off - 1 + cell_h_,
                     cx / tex_w_, cy / tex_h_, (cx + cell_w_) / tex_w_, (cy + cell_h_) / tex_h_,
                     pass == 0 ? shade : argb);
            pen += adv_[g];
        }
    }
    return pen - x;
}

std::vector<std::string> Renderer::wrap(const std::string& s, float max_w) const {
    std::vector<std::string> out;
    std::u32string u = ui::utf8_to_u32(s), line;
    float w = 0;
    size_t last_space = std::u32string::npos;
    for (char32_t c : u) {
        float cw = adv_[glyph_of(c)];
        if (w + cw > max_w && !line.empty()) {
            if (last_space != std::u32string::npos && last_space > 0) {
                out.push_back(ui::u32_to_utf8(line.substr(0, last_space)));
                line = line.substr(last_space + 1);
            } else {
                out.push_back(ui::u32_to_utf8(line));
                line.clear();
            }
            w = 0;
            for (char32_t k : line) w += adv_[glyph_of(k)];
            last_space = std::u32string::npos;
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
