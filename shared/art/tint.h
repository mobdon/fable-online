// Recolours texture pixels by hue, keeping each pixel's lightness, so the shading painted into
// a texture survives. Works on DXT blocks in place (only the two endpoint colours of each block
// change; the per-pixel indices are kept), or on raw BGRA rows. Portable C++17.
#pragma once
#include <cstddef>
#include <cstdint>

namespace fo::art {

// Low-saturation pixels (bare metal, grey cloth) are recoloured; saturated ones (leather,
// brass, rope) are left alone. Dark greys and light greys get separate colours.
struct Tint {
    float metal_hue = 80.0f, metal_sat = 0.36f;   // light and mid greys (degrees, 0..1)
    float dark_hue = 222.0f, dark_sat = 0.32f;    // dark greys
    float dark_below = 0.20f;                     // HSL lightness under which a grey counts as dark
    float keep_sat_above = 0.28f;                 // pixels more saturated than this stay as painted
    float highlight_above = 0.82f;                // brighter greys keep some of their sheen
    float lightness_scale = 1.0f;                 // < 1 darkens recoloured pixels (a black finish)
};

// Named finishes: "green" (the default above) and "black" (a dark dev-only finish).
// Unknown names give the default.
Tint tint_preset(const char* name);

// 0xAARRGGBB in, 0xAARRGGBB out (alpha unchanged).
uint32_t tint_pixel(uint32_t argb, const Tint& t);
// kind: 1 = DXT1, 3 = DXT3, 5 = DXT5. size: bytes of whole blocks.
void tint_dxt(int kind, uint8_t* data, size_t size, const Tint& t);
void tint_bgra(uint8_t* data, size_t size, const Tint& t);

}  // namespace fo::art
