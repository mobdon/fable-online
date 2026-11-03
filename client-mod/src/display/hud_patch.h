#pragma once
// Bigger HUD and menus at high resolutions (portable: builds bytes, never touches memory).
//
// Fable lays out its HUD and menus in screen pixels with fixed-size art, so at 2560x1440
// they cover a quarter of the area they did at 1024x768. Every 2D layout path asks one
// function for the screen size: 0x009BEDC0, `void GetScreenSize(int out[2])` (thiscall on
// the display object; reads +0x194 width, +0x198 height). The patch sends callers outside
// the renderer a smaller "virtual" size (the real one divided by the HUD scale), so the
// game lays the HUD out for that size, anchors and all. The 2D pass then scales everything
// up to the real screen (see hud_scale.cpp), and mouse positions are scaled down to match.
//
// The renderer itself (code from 0x00990000 up, apart from the 2D helpers at 0x009E1D00 to
// 0x009E2430) still gets the real size, so the 3D view keeps its full resolution.
#include <cstdint>
#include <vector>

namespace fo::display {

struct ScreenSizeSite {
    uint32_t func = 0x009BEDC0;
    uint32_t renderer_from = 0x00990000;
    uint32_t gui_helpers_from = 0x009E1D00, gui_helpers_to = 0x009E2430;
};

extern const uint8_t kScreenSizeBytes[12];

// Layout of the block: code, then at kVirtualSizeOffset two int32s (virtual width, height).
// A width of 0 means "no scaling": everyone gets the real size.
constexpr uint32_t kVirtualSizeOffset = 0x60;
std::vector<uint8_t> build_screen_size_cave(uint32_t cave, const ScreenSizeSite& site);
std::vector<uint8_t> build_screen_size_jump(uint32_t cave, const ScreenSizeSite& site);

// The scale the HUD gets at a given screen height: `setting` > 0 is used as is (clamped to
// 1..4); 0 means automatic, so the HUD looks as it would on a 900-pixel-tall screen.
float hud_scale_for(float setting, unsigned screen_h);

}  // namespace fo::display
