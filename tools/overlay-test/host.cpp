// A stand-in for Fable.exe, to test the chat overlay without the game (on Windows or Wine).
// It imports Direct3DCreate9 and DirectInput8Create like Fable does, loads fable_online.dll
// before creating its device, fakes a hero object at the offsets in
// tools/re-notes/02-hero-live-state.md, and renders a floor with a perspective camera.
//   build: see CMakeLists.txt (FO_BUILD_OVERLAY_TEST=ON)
//   run:   overlay_test_host.exe [path\to\fable_online.dll] [seconds] [screenshot.bmp]
// FO_TEST_CHAT=1 opens Realm chat after 2 s, moves the cursor onto the Realm channel and
// clicks it, so the screenshot shows the chat window with its pointer.
#include <windows.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>

namespace {
// Fake hero: vtable values are what the mod's scanner looks for.
struct FakePhysics { uint32_t vtable = 0x0126616C; uint32_t pad[2] = {}; float x = 0, y = 0, z = 0; };
struct FakeHero { uint32_t vtable = 0x012457FC; uint8_t pad[0x5C] = {}; FakePhysics* phys = nullptr; uint8_t pad2[0x84] = {}; float anchor[3] = {7, 7, 7}; };
static_assert(offsetof(FakeHero, phys) == 0x60, "hero+0x60");
static_assert(offsetof(FakeHero, anchor) == 0xE8, "hero+0xE8");
static_assert(offsetof(FakePhysics, x) == 0x0C, "phys+0x0C");

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

D3DMATRIX look_at(float ex, float ey, float ez, float tx, float ty, float tz) {
    // Left-handed, z up in the world.
    float fx = tx - ex, fy = ty - ey, fz = tz - ez, fl = std::sqrt(fx * fx + fy * fy + fz * fz);
    fx /= fl; fy /= fl; fz /= fl;
    float ux = 0, uy = 0, uz = 1;
    float rx = uy * fz - uz * fy, ry = uz * fx - ux * fz, rz = ux * fy - uy * fx, rl = std::sqrt(rx * rx + ry * ry + rz * rz);
    rx /= rl; ry /= rl; rz /= rl;
    ux = fy * rz - fz * ry; uy = fz * rx - fx * rz; uz = fx * ry - fy * rx;
    D3DMATRIX m = {};
    m._11 = rx; m._12 = ux; m._13 = fx;
    m._21 = ry; m._22 = uy; m._23 = fy;
    m._31 = rz; m._32 = uz; m._33 = fz;
    m._41 = -(rx * ex + ry * ey + rz * ez);
    m._42 = -(ux * ex + uy * ey + uz * ez);
    m._43 = -(fx * ex + fy * ey + fz * ez);
    m._44 = 1;
    return m;
}

D3DMATRIX perspective(float fovy, float aspect, float zn, float zf) {
    float ys = 1.0f / std::tan(fovy / 2), xs = ys / aspect;
    D3DMATRIX m = {};
    m._11 = xs; m._22 = ys; m._33 = zf / (zf - zn); m._34 = 1; m._43 = -zn * zf / (zf - zn);
    return m;
}

void save_bmp(IDirect3DDevice9* dev, const char* path) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (sys && SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            FILE* f = std::fopen(path, "wb");
            if (f) {
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih); ih.biWidth = d.Width; ih.biHeight = -static_cast<LONG>(d.Height);
                ih.biPlanes = 1; ih.biBitCount = 32;
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                std::fwrite(&fh, sizeof(fh), 1, f);
                std::fwrite(&ih, sizeof(ih), 1, f);
                for (UINT y = 0; y < d.Height; ++y) std::fwrite(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, 4, d.Width, f);
                std::fclose(f);
                std::printf("screenshot: %s\n", path);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
}
}  // namespace

int main(int argc, char** argv) {
    const char* dll = argc > 1 ? argv[1] : "fable_online.dll";
    int seconds = argc > 2 ? std::atoi(argv[2]) : 20;
    const char* shot = argc > 3 ? argv[3] : nullptr;

    static FakePhysics phys;
    static FakeHero* hero = new FakeHero;  // heap: the scanner walks private memory
    hero->phys = &phys;

    if (!LoadLibraryA(dll)) { std::printf("could not load %s (%lu)\n", dll, GetLastError()); return 1; }
    std::printf("loaded %s\n", dll);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OverlayTestHost";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    HWND w = CreateWindowW(wc.lpszClassName, L"Fable Online overlay test", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           50, 50, 1024, 600, nullptr, nullptr, wc.hInstance, nullptr);

    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.hDeviceWindow = w;
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) {
        std::printf("no Direct3D device\n");
        return 1;
    }

    IDirectInput8W* di = nullptr;
    IDirectInputDevice8W* kb = nullptr;
    if (SUCCEEDED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<void**>(&di), nullptr)))
        di->CreateDevice(GUID_SysKeyboard, &kb, nullptr);
    if (kb) { kb->SetDataFormat(&c_dfDIKeyboard); kb->SetCooperativeLevel(w, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE); kb->Acquire(); }

    struct V { float x, y, z; DWORD c; };
    DWORD start = GetTickCount();
    bool shot_taken = false;
    const bool chat_test = std::getenv("FO_TEST_CHAT") && std::getenv("FO_TEST_CHAT")[0] == '1';
    int chat_step = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        float t = (GetTickCount() - start) / 1000.0f;
        if (t > seconds) break;
        phys.x = std::cos(t * 0.3f) * 2; phys.y = std::sin(t * 0.3f) * 2; phys.z = 0;  // hero walks a circle
        if (kb) { BYTE keys[256]; kb->GetDeviceState(sizeof(keys), keys); }
        if (chat_test) {
            // The Realm channel: the third row of the chat window's left column, which sits
            // at about (60, 646) of a 1280x800 back buffer. The window's client area may be
            // smaller than the back buffer, so aim by fraction.
            RECT rc;
            GetClientRect(w, &rc);
            POINT realm = {rc.right * 60 / 1280, rc.bottom * 646 / 800};
            if (chat_step == 0 && t > 2) { PostMessageW(w, WM_KEYDOWN, VK_RETURN, 0); ++chat_step; }
            if (chat_step == 1 && t > 3) {
                POINT s = realm;
                ClientToScreen(w, &s);
                SetCursorPos(s.x, s.y);
                ++chat_step;
            }
            if (chat_step == 2 && t > 3.5f) {
                POINT c;
                GetCursorPos(&c);
                ScreenToClient(w, &c);
                std::printf("cursor at %ld,%ld; clicking\n", c.x, c.y);
                PostMessageW(w, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(c.x, c.y));
                PostMessageW(w, WM_LBUTTONUP, 0, MAKELPARAM(c.x, c.y));
                ++chat_step;
            }
        }

        dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(40, 60, 50), 1.0f, 0);
        dev->BeginScene();
        D3DMATRIX id = {}; id._11 = id._22 = id._33 = id._44 = 1;
        D3DMATRIX view = look_at(0, -10, 5, 0, 0, 1), proj = perspective(1.0f, 1024.0f / 600.0f, 0.5f, 500.0f);
        dev->SetTransform(D3DTS_WORLD, &id);
        dev->SetTransform(D3DTS_VIEW, &view);
        dev->SetTransform(D3DTS_PROJECTION, &proj);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
        V floor[] = {{-10, -10, 0, 0xFF507050}, {10, -10, 0, 0xFF507050}, {-10, 10, 0, 0xFF406040},
                     {10, -10, 0, 0xFF507050}, {10, 10, 0, 0xFF406040}, {-10, 10, 0, 0xFF406040}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, floor, sizeof(V));
        // A pole where the hero is, so you can see whether the tag sits above it.
        V pole[] = {{phys.x - 0.1f, phys.y, 0, 0xFFC08040}, {phys.x + 0.1f, phys.y, 0, 0xFFC08040}, {phys.x, phys.y, 1.9f, 0xFFFFC080}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, pole, sizeof(V));
        // The HUD pass: an orthographic projection, which the mod must ignore.
        D3DMATRIX ortho = {}; ortho._11 = 2.0f / 1024; ortho._22 = 2.0f / 600; ortho._33 = 1; ortho._44 = 1;
        dev->SetTransform(D3DTS_PROJECTION, &ortho);
        dev->EndScene();
        if (shot && !shot_taken && t > seconds - 1.5f) { save_bmp(dev, shot); shot_taken = true; }
        dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(16);
    }
    std::printf("done\n");
    return 0;
}
