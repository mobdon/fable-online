#include "tint.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fo::art {
namespace {

struct Hsl { float h, s, l; };

Hsl to_hsl(float r, float g, float b) {
    float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    float l = (mx + mn) * 0.5f, d = mx - mn;
    if (d < 1e-6f) return {0.0f, 0.0f, l};
    float s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
    float h;
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    return {h * 60.0f, s, l};
}

float hue_part(float p, float q, float t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

void from_hsl(Hsl c, float& r, float& g, float& b) {
    if (c.s <= 0) { r = g = b = c.l; return; }
    float q = c.l < 0.5f ? c.l * (1 + c.s) : c.l + c.s - c.l * c.s;
    float p = 2 * c.l - q, h = c.h / 360.0f;
    r = hue_part(p, q, h + 1.0f / 3);
    g = hue_part(p, q, h);
    b = hue_part(p, q, h - 1.0f / 3);
}

uint8_t to8(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

uint32_t from565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return 0xFF000000u | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
}

uint16_t to565(uint32_t argb) {
    uint32_t r = (argb >> 16) & 255, g = (argb >> 8) & 255, b = argb & 255;
    return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
void wr16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }

// One colour block: two RGB565 endpoints then 16 two-bit indices.
void tint_block(uint8_t* b, bool dxt1, const Tint& t) {
    uint16_t c0 = rd16(b), c1 = rd16(b + 2);
    uint16_t n0 = to565(tint_pixel(from565(c0), t)), n1 = to565(tint_pixel(from565(c1), t));
    uint32_t idx;
    std::memcpy(&idx, b + 4, 4);
    if (dxt1) {
        // DXT1 picks its mode from the endpoint order: c0 > c1 is four colours, otherwise
        // three colours plus transparent. Keep the block's mode, swapping endpoints if needed.
        bool four = c0 > c1;
        if (four) {
            if (n0 == n1) { if (n0 < 0xFFFF) ++n0; else --n1; }
            if (n0 < n1) { std::swap(n0, n1); idx ^= 0x55555555u; }  // 0<->1, 2<->3
        } else if (n0 > n1) {
            std::swap(n0, n1);
            uint32_t flip = 0;
            for (int i = 0; i < 16; ++i)
                if (((idx >> (2 * i)) & 3) < 2) flip |= 1u << (2 * i);  // 0<->1 only
            idx ^= flip;
        }
    }
    wr16(b, n0);
    wr16(b + 2, n1);
    std::memcpy(b + 4, &idx, 4);
}

}  // namespace

uint32_t tint_pixel(uint32_t argb, const Tint& t) {
    float r = ((argb >> 16) & 255) / 255.0f, g = ((argb >> 8) & 255) / 255.0f, b = (argb & 255) / 255.0f;
    Hsl c = to_hsl(r, g, b);
    if (c.s > t.keep_sat_above) return argb;
    Hsl n{};
    n.l = c.l;
    if (c.l < t.dark_below) {
        n.h = t.dark_hue;
        n.s = t.dark_sat;
    } else {
        n.h = t.metal_hue;
        n.s = t.metal_sat;
        if (c.l > t.highlight_above) n.s *= std::max(0.0f, (1.0f - c.l) / (1.0f - t.highlight_above));
    }
    float nr, ng, nb;
    from_hsl(n, nr, ng, nb);
    return (argb & 0xFF000000u) | (uint32_t(to8(nr)) << 16) | (uint32_t(to8(ng)) << 8) | to8(nb);
}

void tint_dxt(int kind, uint8_t* data, size_t size, const Tint& t) {
    size_t step = kind == 1 ? 8 : 16, colour = kind == 1 ? 0 : 8;
    for (size_t p = 0; p + step <= size; p += step) tint_block(data + p + colour, kind == 1, t);
}

void tint_bgra(uint8_t* data, size_t size, const Tint& t) {
    for (size_t p = 0; p + 4 <= size; p += 4) {
        uint32_t argb = uint32_t(data[p + 3]) << 24 | uint32_t(data[p + 2]) << 16 | uint32_t(data[p + 1]) << 8 | data[p];
        uint32_t o = tint_pixel(argb, t);
        data[p] = static_cast<uint8_t>(o);
        data[p + 1] = static_cast<uint8_t>(o >> 8);
        data[p + 2] = static_cast<uint8_t>(o >> 16);
    }
}

}  // namespace fo::art
