#include "camera_patch.h"
#include <cmath>
#include <cstring>

namespace fo::display {

const uint8_t kHookBytes[7] = {0x8A, 0x43, 0x54, 0x84, 0xC0, 0x74, 0x36};
const uint8_t kAutoBytes[4] = {0xD9, 0x44, 0x24, 0x14};
const uint8_t kDoneBytes[6] = {0x8D, 0x85, 0x80, 0x00, 0x00, 0x00};

namespace {
struct Emit {
    std::vector<uint8_t> b;
    uint32_t base;
    void u8(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    void f32(float f) { uint32_t v; std::memcpy(&v, &f, 4); u32(v); }
    uint32_t here() const { return base + static_cast<uint32_t>(b.size()); }
    void rel32(uint32_t target) { u32(target - (here() + 4)); }
};
}  // namespace

std::vector<uint8_t> build_camera_cave(uint32_t cave, const CameraSite& s, float fov_scale) {
    // Constants after the code, at fixed offsets.
    const uint32_t c_limit = cave + 0x50, c_half = cave + 0x54, c_k = cave + kCaveFovConstOffset, c_one = cave + 0x5C;
    Emit e{{}, cave};
    // Entry: FPU st0 = target aspect (w/h); ebx = camera settings; ebp = camera; [esp+1Ch] = fov.
    e.u8({0x80, 0x7B, 0x54, 0x00});              // cmp byte [ebx+54h], 0   (explicit x/y angles?)
    e.u8({0x0F, 0x85}); e.rel32(s.explicit_fov); // jne original explicit path
    e.u8({0xD8, 0x15}); e.u32(c_limit);          // fcom dword [limit]
    e.u8({0xDF, 0xE0});                          // fnstsw ax
    e.u8({0xF6, 0xC4, 0x41});                    // test ah, 41h            (aspect <= 4:3?)
    e.u8({0x0F, 0x85}); e.rel32(s.auto_fov);     // jne original automatic path
    e.u8({0xD9, 0x44, 0x24, 0x1C});              // fld dword [esp+1Ch]     fov
    e.u8({0xD8, 0x0D}); e.u32(c_half);           // fmul dword [0.5]
    e.u8({0xD9, 0xF2});                          // fptan
    e.u8({0xDD, 0xD8});                          // fstp st0                tan(fov/2), aspect
    e.u8({0xD8, 0x0D}); e.u32(c_k);              // fmul dword [0.75*scale] vertical tangent
    e.u8({0xD8, 0x3D}); e.u32(c_one);            // fdivr dword [1.0]       ys
    e.u8({0xD9, 0x95, 0xD8, 0x00, 0x00, 0x00});  // fst dword [ebp+0D8h]
    e.u8({0xD8, 0xF1});                          // fdiv st0, st1           xs = ys / aspect
    e.u8({0xD9, 0x9D, 0xD4, 0x00, 0x00, 0x00});  // fstp dword [ebp+0D4h]
    e.u8({0xDD, 0xD8});                          // fstp st0                pop aspect
    e.u8({0xE9}); e.rel32(s.done);               // jmp back
    while (e.b.size() < 0x50) e.u8({0xCC});
    e.f32(4.0f / 3.0f + 0.001f);                 // limit
    e.f32(0.5f);                                 // half
    e.f32(0.75f * fov_scale);                    // k
    e.f32(1.0f);                                 // one
    return e.b;
}

std::vector<uint8_t> build_camera_jump(uint32_t cave, const CameraSite& s) {
    Emit e{{}, s.hook};
    e.u8({0xE9}); e.rel32(cave);
    e.u8({0x90, 0x90});
    return e.b;
}

Scales fable_original(float fov, float w, float h) {
    float a = w / h;
    float xs = 1.0f / std::tan((0.5f * (0.75f - h / w) + 1.0f) * fov * 0.5f);
    return {xs, xs * a};
}

Scales hor_plus(float fov, float w, float h, float fov_scale) {
    float a = w / h;
    if (a <= 4.0f / 3.0f + 0.001f) return fable_original(fov, w, h);
    float ys = 1.0f / (std::tan(fov * 0.5f) * 0.75f * fov_scale);
    return {ys / a, ys};
}

}  // namespace fo::display
