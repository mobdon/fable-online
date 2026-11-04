#include "frame_probe.h"
#include "../util/compiler.h"
#if __has_include(<d3dx9core.h>)
#include <d3dx9core.h>
#else
// The Windows SDK no longer ships D3DX (only the legacy DirectX SDK and mingw-w64 do).
// The probe loads D3DXDisassembleShader at run time from d3dx9_25.dll, so all it needs
// from the header is the buffer interface that call hands back.
struct ID3DXBuffer : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual DWORD STDMETHODCALLTYPE GetBufferSize() = 0;
};
#endif
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "../config.h"
#include "../log.h"
#include "../overlay/patch.h"

namespace fo::display::probe {
namespace {
constexpr int kSetRenderTarget = 37, kSetViewport = 47, kDrawPrimitive = 81, kDrawIndexedPrimitive = 82,
              kDrawPrimitiveUP = 83, kDrawIndexedPrimitiveUP = 84, kSetVertexDeclaration = 87, kSetFVF = 89,
              kSetVertexShader = 92, kSetVertexShaderConstantF = 94,
              kSetRenderState = 57, kSetTexture = 65, kSetPixelShader = 107, kSetPixelShaderConstantF = 109;

using SetRenderTarget_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetViewport_t = HRESULT(WINAPI*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using DrawPrimitive_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using DrawIndexedPrimitive_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, const void*, UINT);
using DrawIndexedPrimitiveUP_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT, UINT, const void*, D3DFORMAT, const void*, UINT);
using SetVertexDeclaration_t = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexDeclaration9*);
using SetFVF_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD);
using SetVertexShader_t = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetVertexShaderConstantF_t = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using SetRenderState_t = HRESULT(WINAPI*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
using SetTexture_t = HRESULT(WINAPI*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using SetPixelShader_t = HRESULT(WINAPI*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetPixelShaderConstantF_t = HRESULT(WINAPI*)(IDirect3DDevice9*, UINT, const float*, UINT);
using Disassemble_t = HRESULT(WINAPI*)(const DWORD*, BOOL, LPCSTR, ID3DXBuffer**);

SetRenderTarget_t o_SetRenderTarget;
SetViewport_t o_SetViewport;
DrawPrimitive_t o_DrawPrimitive;
DrawIndexedPrimitive_t o_DrawIndexedPrimitive;
DrawPrimitiveUP_t o_DrawPrimitiveUP;
DrawIndexedPrimitiveUP_t o_DrawIndexedPrimitiveUP;
SetVertexDeclaration_t o_SetVertexDeclaration;
SetFVF_t o_SetFVF;
SetVertexShader_t o_SetVertexShader;
SetVertexShaderConstantF_t o_SetVertexShaderConstantF;
SetRenderState_t o_SetRenderState;
SetTexture_t o_SetTexture;
SetPixelShader_t o_SetPixelShader;
SetPixelShaderConstantF_t o_SetPixelShaderConstantF;

// Render thread only.
bool g_on = false;
int g_lines = 0;
constexpr int kMaxLines = 30000;
IDirect3DVertexShader9* g_vs = nullptr;
std::vector<IDirect3DVertexShader9*> g_shaders;   // seen this capture, in order
std::vector<IDirect3DVertexDeclaration9*> g_decls;
bool g_key_was_down = false;
int g_request_frames = 0;
bool g_ready = false;

int shader_id(IDirect3DVertexShader9* s) {
    if (!s) return -1;
    for (size_t i = 0; i < g_shaders.size(); ++i) if (g_shaders[i] == s) return static_cast<int>(i);
    s->AddRef();
    g_shaders.push_back(s);
    return static_cast<int>(g_shaders.size() - 1);
}

void line(const char* fmt, ...) {
    if (!g_on || g_lines >= kMaxLines) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fo::log("probe: %s", buf);
    if (++g_lines == kMaxLines) fo::log("probe: (line limit reached)");
}

const char* usage_name(BYTE u) {
    static const char* n[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
                              "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    return u < 14 ? n[u] : "?";
}

void log_decl(IDirect3DVertexDeclaration9* d) {
    if (!d) return;
    for (auto* x : g_decls) if (x == d) return;
    d->AddRef();
    g_decls.push_back(d);
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    if (FAILED(d->GetDeclaration(el, &n))) return;
    std::string s;
    for (UINT i = 0; i < n && el[i].Stream != 0xFF; ++i) {
        char b[64];
        snprintf(b, sizeof b, "%s%u@s%u+%u ", usage_name(el[i].Usage), el[i].UsageIndex, el[i].Stream, el[i].Offset);
        s += b;
    }
    line("  decl %p: %s", static_cast<void*>(d), s.c_str());
}

void dump_shaders() {
    HMODULE dx = GetModuleHandleW(L"d3dx9_25.dll");
    if (!dx) dx = LoadLibraryW(L"d3dx9_25.dll");
    auto dis = dx ? reinterpret_cast<Disassemble_t>(GetProcAddress(dx, "D3DXDisassembleShader")) : nullptr;
    for (size_t i = 0; i < g_shaders.size() && i < 80; ++i) {
        UINT size = 0;
        g_shaders[i]->GetFunction(nullptr, &size);
        std::vector<DWORD> code((size + 3) / 4);
        if (!size || FAILED(g_shaders[i]->GetFunction(code.data(), &size))) continue;
        fo::log("probe: ---- vertex shader #%u (%u bytes) ----", static_cast<unsigned>(i), size);
        ID3DXBuffer* text = nullptr;
        if (dis && SUCCEEDED(dis(code.data(), FALSE, nullptr, &text)) && text) {
            std::string all(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
            size_t at = 0;
            int n = 0;
            while (at < all.size() && n++ < 200) {
                size_t nl = all.find('\n', at);
                std::string l = all.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
                if (!l.empty() && l[0] != '\0') fo::log("probe:   %s", l.c_str());
                if (nl == std::string::npos) break;
                at = nl + 1;
            }
            text->Release();
        } else {
            fo::log("probe:   (no disassembler)");
        }
    }
    for (auto* s : g_shaders) s->Release();
    for (auto* d : g_decls) d->Release();
    g_shaders.clear();
    g_decls.clear();
}

void draw_line(const char* kind, unsigned prims, const void* up, UINT stride, void* caller) {
    if (!g_on) return;
    // The draw's caller plus the next few return addresses into Fable.exe found on the stack:
    // enough to tell which game code drew it (HUD, minimap, loading screen, highlights).
    char chain[200];
    int n = snprintf(chain, sizeof chain, "%p", caller);
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(FO_FRAME_ADDRESS());
    MEMORY_BASIC_INFORMATION mbi = {};
    if (VirtualQuery(sp, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
        const uint32_t* end = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
        int found = 0;
        for (const uint32_t* p = sp; p < end && p < sp + 160 && found < 5 && n < 180; ++p) {
            const uint32_t v = *p;
            if (v > 0x00401000 && v < 0x00E00000 && v != reinterpret_cast<uintptr_t>(caller)) {
                n += snprintf(chain + n, sizeof chain - n, " %08X", v);
                ++found;
            }
        }
    }
    line("  from %s", chain);
    char extra[160] = "";
    if (up && stride >= 16) {
        const float* f = static_cast<const float*>(up);
        snprintf(extra, sizeof extra, " v0=(%.1f %.1f %.3f %.3f)", f[0], f[1], f[2], f[3]);
    }
    line("%s prims=%u vs=#%d%s", kind, prims, shader_id(g_vs), extra);
}

HRESULT WINAPI hk_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    if (g_on) {
        D3DSURFACE_DESC desc = {};
        if (s) s->GetDesc(&desc);
        line("SetRenderTarget %lu %p %ux%u", i, static_cast<void*>(s), desc.Width, desc.Height);
    }
    return o_SetRenderTarget(d, i, s);
}
HRESULT WINAPI hk_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    if (g_on && v) line("SetViewport %lu,%lu %lux%lu", v->X, v->Y, v->Width, v->Height);
    return o_SetViewport(d, v);
}
HRESULT WINAPI hk_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT a, UINT n) {
    draw_line("DrawPrimitive", n, nullptr, 0, FO_RETURN_ADDRESS());
    return o_DrawPrimitive(d, t, a, n);
}
HRESULT WINAPI hk_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT b, UINT m, UINT nv, UINT s, UINT n) {
    draw_line("DrawIndexedPrimitive", n, nullptr, 0, FO_RETURN_ADDRESS());
    return o_DrawIndexedPrimitive(d, t, b, m, nv, s, n);
}
HRESULT WINAPI hk_DrawPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT n, const void* v, UINT stride) {
    draw_line("DrawPrimitiveUP", n, v, stride, FO_RETURN_ADDRESS());
    return o_DrawPrimitiveUP(d, t, n, v, stride);
}
HRESULT WINAPI hk_DrawIndexedPrimitiveUP(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT mi, UINT nv, UINT n, const void* i,
                                         D3DFORMAT f, const void* v, UINT stride) {
    draw_line("DrawIndexedPrimitiveUP", n, v, stride, FO_RETURN_ADDRESS());
    return o_DrawIndexedPrimitiveUP(d, t, mi, nv, n, i, f, v, stride);
}
HRESULT WINAPI hk_SetVertexDeclaration(IDirect3DDevice9* d, IDirect3DVertexDeclaration9* decl) {
    if (g_on) { line("SetVertexDeclaration %p", static_cast<void*>(decl)); log_decl(decl); }
    return o_SetVertexDeclaration(d, decl);
}
HRESULT WINAPI hk_SetFVF(IDirect3DDevice9* d, DWORD fvf) {
    if (g_on) line("SetFVF 0x%lX%s", fvf, (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW ? " (screen-space XYZRHW)" : "");
    return o_SetFVF(d, fvf);
}
HRESULT WINAPI hk_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    g_vs = s;
    if (g_on) line("SetVertexShader #%d", shader_id(s));
    return o_SetVertexShader(d, s);
}
HRESULT WINAPI hk_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT start, const float* c, UINT n) {
    if (g_on && c) {
        for (UINT i = 0; i < n && i < 6; ++i)
            line("  c%u = %.5g %.5g %.5g %.5g", start + i, c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
        if (n > 6) line("  (+%u more from c%u)", n - 6, start + 6);
    }
    return o_SetVertexShaderConstantF(d, start, c, n);
}

HRESULT WINAPI hk_SetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE st, DWORD v) {
    // Colours and blending: what decides a tint such as the target highlight.
    if (g_on && (st == D3DRS_TEXTUREFACTOR || st == D3DRS_SRCBLEND || st == D3DRS_DESTBLEND || st == D3DRS_ALPHABLENDENABLE ||
                 st == D3DRS_AMBIENT))
        line("SetRenderState %d = 0x%08lX", static_cast<int>(st), static_cast<unsigned long>(v));
    return o_SetRenderState(d, st, v);
}
HRESULT WINAPI hk_SetTexture(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) {
    if (g_on) line("SetTexture %lu %p", static_cast<unsigned long>(stage), static_cast<void*>(t));
    return o_SetTexture(d, stage, t);
}
HRESULT WINAPI hk_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps) {
    if (g_on) line("SetPixelShader %p", static_cast<void*>(ps));
    return o_SetPixelShader(d, ps);
}
HRESULT WINAPI hk_SetPixelShaderConstantF(IDirect3DDevice9* d, UINT start, const float* c, UINT n) {
    if (g_on && c)
        for (UINT i = 0; i < n && i < 8; ++i)
            line("  pc%u = %.5g %.5g %.5g %.5g", start + i, c[i * 4], c[i * 4 + 1], c[i * 4 + 2], c[i * 4 + 3]);
    return o_SetPixelShaderConstantF(d, start, c, n);
}

template <typename T>
void slot(IDirect3DDevice9* dev, int index, T hook, T& orig) {
    if (auto o = fo::patch::vtable(dev, index, reinterpret_cast<void*>(hook))) orig = reinterpret_cast<T>(o);
}
}  // namespace

void hook(IDirect3DDevice9* dev) {
    slot(dev, kSetRenderTarget, hk_SetRenderTarget, o_SetRenderTarget);
    slot(dev, kSetViewport, hk_SetViewport, o_SetViewport);
    slot(dev, kDrawPrimitive, hk_DrawPrimitive, o_DrawPrimitive);
    slot(dev, kDrawIndexedPrimitive, hk_DrawIndexedPrimitive, o_DrawIndexedPrimitive);
    slot(dev, kDrawPrimitiveUP, hk_DrawPrimitiveUP, o_DrawPrimitiveUP);
    slot(dev, kDrawIndexedPrimitiveUP, hk_DrawIndexedPrimitiveUP, o_DrawIndexedPrimitiveUP);
    slot(dev, kSetVertexDeclaration, hk_SetVertexDeclaration, o_SetVertexDeclaration);
    slot(dev, kSetFVF, hk_SetFVF, o_SetFVF);
    slot(dev, kSetVertexShader, hk_SetVertexShader, o_SetVertexShader);
    slot(dev, kSetVertexShaderConstantF, hk_SetVertexShaderConstantF, o_SetVertexShaderConstantF);
    slot(dev, kSetRenderState, hk_SetRenderState, o_SetRenderState);
    slot(dev, kSetTexture, hk_SetTexture, o_SetTexture);
    slot(dev, kSetPixelShader, hk_SetPixelShader, o_SetPixelShader);
    slot(dev, kSetPixelShaderConstantF, hk_SetPixelShaderConstantF, o_SetPixelShaderConstantF);
    g_ready = true;
    fo::log("probe: frame probe ready (Mod settings > Developer > Capture one frame%s)", config().dev_hotkeys ? ", or F10" : "");
}

void on_present(IDirect3DDevice9* dev) {
    if (g_on) {
        g_on = false;
        fo::log("probe: ==== end of frame (%d lines) ====", g_lines);
        dump_shaders();
        fo::log("probe: capture done");
        return;
    }
    bool down = config().dev_hotkeys && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    bool asked = false;
    if (g_request_frames > 0 && --g_request_frames == 0) asked = true;
    if ((down && !g_key_was_down) || asked) {
        g_on = true;
        g_lines = 0;
        D3DVIEWPORT9 vp = {};
        dev->GetViewport(&vp);
        fo::log("probe: ==== frame capture (viewport %lux%lu) ====", vp.Width, vp.Height);
        line("current vertex shader #%d", shader_id(g_vs));
    }
    g_key_was_down = down;
}

void request_capture() { g_request_frames = 3; }   // a few frames on, once the menu is gone
bool ready() { return g_ready; }

void note(const char* what) { line("-- %s", what); }

void on_transform(D3DTRANSFORMSTATETYPE st, const D3DMATRIX* m) {
    if (!g_on || !m) return;
    line("SetTransform %d: [%.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g]",
         static_cast<int>(st), m->_11, m->_12, m->_13, m->_14, m->_21, m->_22, m->_23, m->_24, m->_31, m->_32, m->_33,
         m->_34, m->_41, m->_42, m->_43, m->_44);
}
}  // namespace fo::display::probe
