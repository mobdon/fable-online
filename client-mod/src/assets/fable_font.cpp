#include "fable_font.h"
#include <cmath>
#include <cstring>

namespace fo::assets {
namespace {
uint32_t u32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t u16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
float f32(const uint8_t* p) { uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }
bool fail(std::string* e, const char* why) { if (e) *e = why; return false; }
}  // namespace

bool parse_font(const std::vector<uint8_t>& d, Font& out, std::string* error) {
    out = Font{};
    size_t p = 0;
    while (p < d.size() && p < 64 && d[p]) ++p;
    if (p >= d.size() || d[p]) return fail(error, "no face name");
    out.face.assign(reinterpret_cast<const char*>(d.data()), p);
    ++p;
    // u32 height, u32 weight, u8 italic, u32 line, tex_w, tex_h, first, last, chunk_count
    if (p + 33 > d.size()) return fail(error, "short header");
    out.height = static_cast<int>(u32(&d[p]));
    p += 9;
    out.line = static_cast<int>(u32(&d[p]));
    out.tex_w = static_cast<int>(u32(&d[p + 4]));
    out.tex_h = static_cast<int>(u32(&d[p + 8]));
    uint32_t first = u32(&d[p + 12]), last = u32(&d[p + 16]), chunks = u32(&d[p + 20]);
    p += 24;
    if (out.tex_w < 8 || out.tex_h < 8 || out.tex_w > 2048 || out.tex_h > 2048 || first > 255 || last > 255 ||
        first > last || chunks == 0 || chunks > 16 || out.line <= 0 || out.line > 128)
        return fail(error, "bad header");
    size_t tex_bytes = static_cast<size_t>(out.tex_w) * out.tex_h * 4;
    if (d.size() < tex_bytes) return fail(error, "no texture");
    size_t tex_at = d.size() - tex_bytes;

    // UVs are normalised by (size - 1), not size.
    const float sx = static_cast<float>(out.tex_w - 1), sy = static_cast<float>(out.tex_h - 1);
    uint32_t ch = first;
    for (uint32_t c = 0; c < chunks; ++c) {
        if (p + 12 > tex_at) return fail(error, "short chunk");
        uint32_t count = u32(&d[p + 8]);
        p += 12;
        if (count > 256 || p + count * 22ull > tex_at) return fail(error, "bad chunk");
        for (uint32_t g = 0; g < count; ++g, ++ch, p += 22) {
            if (ch > 255) continue;
            Glyph& gl = out.glyphs[ch];
            gl.x0 = static_cast<int>(std::lround(f32(&d[p]) * sx));
            gl.y0 = static_cast<int>(std::lround(f32(&d[p + 4]) * sy));
            gl.x1 = static_cast<int>(std::lround(f32(&d[p + 8]) * sx));
            gl.y1 = static_cast<int>(std::lround(f32(&d[p + 12]) * sy));
            gl.bearing = static_cast<int16_t>(u16(&d[p + 16]));
            gl.width = u16(&d[p + 18]);
            gl.advance = u16(&d[p + 20]);
            // Some banks list a glyph past the bottom of their texture (Arial 18 'z'): skip it.
            gl.valid = gl.x0 >= 0 && gl.y0 >= 0 && gl.x1 <= out.tex_w && gl.y1 <= out.tex_h && gl.x1 >= gl.x0 &&
                       gl.y1 >= gl.y0 && gl.advance < 128;
        }
    }
    out.argb.resize(static_cast<size_t>(out.tex_w) * out.tex_h);
    std::memcpy(out.argb.data(), &d[tex_at], tex_bytes);  // BGRA bytes == 0xAARRGGBB little-endian
    return true;
}

}  // namespace fo::assets
