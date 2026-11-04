#include "hud_patch.h"
#include <cmath>
#include <stdexcept>

namespace fo::display {

// mov edx,[ecx+198h] / mov ecx,[ecx+194h]
const uint8_t kScreenSizeBytes[12] = {0x8B, 0x91, 0x98, 0x01, 0x00, 0x00, 0x8B, 0x89, 0x94, 0x01, 0x00, 0x00};

namespace {
struct Emit {
    std::vector<uint8_t> b;
    uint32_t base;
    void u8(std::initializer_list<uint8_t> v) { b.insert(b.end(), v); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
    uint32_t here() const { return base + static_cast<uint32_t>(b.size()); }
    // Short forward jump with its offset patched later.
    size_t jcc8(uint8_t op) { u8({op, 0x00}); return b.size() - 1; }
    void land(size_t at) { b[at] = static_cast<uint8_t>(b.size() - (at + 1)); }
};
}  // namespace

std::vector<uint8_t> build_screen_size_cave(uint32_t cave, const ScreenSizeSite& s) {
    const uint32_t vw = cave + kVirtualSizeOffset, vh = vw + 4;
    Emit e{{}, cave};
    e.u8({0x83, 0x3D}); e.u32(vw); e.u8({0x00});       // cmp dword [vw], 0
    size_t to_real0 = e.jcc8(0x74);                    // je real
    e.u8({0x8B, 0x04, 0x24});                          // mov eax, [esp]   (return address)
    e.u8({0x3D}); e.u32(s.renderer_from);              // cmp eax, renderer_from
    size_t to_virt0 = e.jcc8(0x72);                    // jb virtual       (game and GUI code)
    e.u8({0x3D}); e.u32(s.gui_helpers_from);           // cmp eax, helpers_from
    size_t to_list = e.jcc8(0x72);                     // jb list          (renderer)
    e.u8({0x3D}); e.u32(s.gui_helpers_to);             // cmp eax, helpers_to
    size_t to_virt1 = e.jcc8(0x72);                    // jb virtual       (2D helpers)
    // list: renderer code that converts layout pixels to clip space
    e.land(to_list);
    std::vector<size_t> to_virt_list;
    for (uint32_t ret : s.layout_callers) {
        e.u8({0x3D}); e.u32(ret);                      // cmp eax, ret
        to_virt_list.push_back(e.jcc8(0x74));          // je virtual
    }
    // real:
    e.land(to_real0);
    e.u8({0x8B, 0x91, 0x98, 0x01, 0x00, 0x00});        // mov edx, [ecx+198h]
    e.u8({0x8B, 0x89, 0x94, 0x01, 0x00, 0x00});        // mov ecx, [ecx+194h]
    size_t to_store = e.jcc8(0xEB);                    // jmp store
    // virtual:
    e.land(to_virt0); e.land(to_virt1);
    for (size_t at : to_virt_list) e.land(at);
    e.u8({0x8B, 0x15}); e.u32(vh);                     // mov edx, [vh]
    e.u8({0x8B, 0x0D}); e.u32(vw);                     // mov ecx, [vw]
    // store:
    e.land(to_store);
    e.u8({0x8B, 0x44, 0x24, 0x04});                    // mov eax, [esp+4]
    e.u8({0x89, 0x08});                                // mov [eax], ecx
    e.u8({0x89, 0x50, 0x04});                          // mov [eax+4], edx
    e.u8({0xC2, 0x04, 0x00});                          // ret 4
    if (e.b.size() > kVirtualSizeOffset) throw std::logic_error("screen-size code block too long");
    while (e.b.size() < kVirtualSizeOffset) e.u8({0xCC});
    e.u32(0); e.u32(0);                                // virtual width, height
    return e.b;
}

std::vector<uint8_t> build_screen_size_jump(uint32_t cave, const ScreenSizeSite& s) {
    std::vector<uint8_t> b = {0xE9};
    uint32_t rel = cave - (s.func + 5);
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(rel >> (8 * i)));
    b.push_back(0x90);
    return b;   // 6 bytes over the first mov
}

float hud_scale_for(float setting, unsigned screen_h) {
    float s = setting > 0 ? setting : static_cast<float>(screen_h) / 900.0f;
    if (!(s >= 1.0f)) s = 1.0f;
    if (s > 4.0f) s = 4.0f;
    return s;
}

}  // namespace fo::display
