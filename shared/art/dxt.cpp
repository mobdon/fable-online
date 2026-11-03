#include "dxt.h"
#include <cstring>

namespace fo::art {

namespace {

uint32_t rgb565(uint16_t c) {
    uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return ((r * 255 + 15) / 31) << 16 | ((g * 255 + 31) / 63) << 8 | ((b * 255 + 15) / 31);
}

void color_block(const uint8_t* b, bool four_colors_always, uint32_t out[16]) {
    uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
    uint32_t p[4];
    p[0] = rgb565(c0);
    p[1] = rgb565(c1);
    auto mix = [](uint32_t a, uint32_t b2, int wa, int wb, int d) {
        uint32_t r = (((a >> 16) & 255) * wa + ((b2 >> 16) & 255) * wb) / d;
        uint32_t g = (((a >> 8) & 255) * wa + ((b2 >> 8) & 255) * wb) / d;
        uint32_t bl = ((a & 255) * wa + (b2 & 255) * wb) / d;
        return r << 16 | g << 8 | bl;
    };
    bool transparent = false;
    if (c0 > c1 || four_colors_always) {
        p[2] = mix(p[0], p[1], 2, 1, 3);
        p[3] = mix(p[0], p[1], 1, 2, 3);
    } else {
        p[2] = mix(p[0], p[1], 1, 1, 2);
        p[3] = 0;
        transparent = true;
    }
    uint32_t bits = uint32_t(b[4]) | uint32_t(b[5]) << 8 | uint32_t(b[6]) << 16 | uint32_t(b[7]) << 24;
    for (int i = 0; i < 16; ++i) {
        uint32_t idx = (bits >> (2 * i)) & 3;
        out[i] = p[idx] | ((transparent && idx == 3) ? 0u : 0xFF000000u);
    }
}

}  // namespace

void decode_dxt(int kind, const uint8_t* data, size_t size, int w, int rows, std::vector<uint32_t>& out) {
    out.assign(size_t(w) * rows, 0);
    size_t block = kind == 1 ? 8 : 16;
    int bw = (w + 3) / 4, bh = (rows + 3) / 4;
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            size_t off = (size_t(by) * bw + bx) * block;
            if (off + block > size) return;
            const uint8_t* b = data + off;
            uint32_t px[16];
            if (kind == 1) {
                color_block(b, false, px);
            } else {
                color_block(b + 8, true, px);
                uint8_t a[16];
                if (kind == 3) {
                    for (int i = 0; i < 16; ++i) { int v = (b[i / 2] >> ((i & 1) * 4)) & 15; a[i] = uint8_t(v * 17); }
                } else {
                    uint8_t a0 = b[0], a1 = b[1], pal[8] = {a0, a1};
                    if (a0 > a1) for (int i = 1; i < 7; ++i) pal[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
                    else { for (int i = 1; i < 5; ++i) pal[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5); pal[6] = 0; pal[7] = 255; }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; ++i) a[i] = pal[(bits >> (3 * i)) & 7];
                }
                for (int i = 0; i < 16; ++i) px[i] = (px[i] & 0xFFFFFF) | uint32_t(a[i]) << 24;
            }
            for (int i = 0; i < 16; ++i) {
                int x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x < w && y < rows) out[size_t(y) * w + x] = px[i];
            }
        }
}

bool decode(const Texture& t, Image& out) {
    const std::vector<uint8_t>& raw = t.mip0;
    if (t.width == 0 || raw.empty()) return false;
    int w = t.width;
    Image img;
    img.w = w;
    if (t.pixels == PixelFormat::DXT1) {
        img.h = int(raw.size() * 2 / w) & ~3;
        decode_dxt(1, raw.data(), raw.size(), w, img.h, img.px);
    } else if (t.pixels == PixelFormat::DXT3 || t.pixels == PixelFormat::DXT5) {
        img.h = int(raw.size() / w) & ~3;
        decode_dxt(t.pixels == PixelFormat::DXT5 ? 5 : 3, raw.data(), raw.size(), w, img.h, img.px);
    } else {
        img.h = int(raw.size() / (size_t(w) * 4));
        img.px.resize(size_t(w) * img.h);
        std::memcpy(img.px.data(), raw.data(), img.px.size() * 4);  // BGRA bytes == 0xAARRGGBB little-endian
    }
    int fw = t.frame_w, fh = t.frame_h;
    if (fw && fh && fw <= img.w && fh <= img.h && (fw != img.w || fh != img.h)) {
        Image c;
        c.w = fw;
        c.h = fh;
        c.px.resize(size_t(fw) * fh);
        for (int y = 0; y < fh; ++y) std::memcpy(&c.px[size_t(y) * fw], &img.px[size_t(y) * img.w], size_t(fw) * 4);
        img = std::move(c);
    }
    out = std::move(img);
    return true;
}

Image resize(const Image& src, int w, int h) {
    Image d;
    if (src.empty() || w <= 0 || h <= 0) return d;
    d.w = w;
    d.h = h;
    d.px.resize(size_t(w) * h);
    // Box filter when shrinking (avoids shimmer), bilinear when growing. Premultiplied maths.
    float sx = float(src.w) / w, sy = float(src.h) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float acc[4] = {0, 0, 0, 0}, wsum = 0;
            float fx0 = x * sx, fy0 = y * sy;
            if (sx > 1 || sy > 1) {
                int x0 = int(fx0), x1 = int((x + 1) * sx), y0 = int(fy0), y1 = int((y + 1) * sy);
                if (x1 <= x0) x1 = x0 + 1;
                if (y1 <= y0) y1 = y0 + 1;
                for (int yy = y0; yy < y1 && yy < src.h; ++yy)
                    for (int xx = x0; xx < x1 && xx < src.w; ++xx) {
                        uint32_t c = src.px[size_t(yy) * src.w + xx];
                        float a = float(c >> 24) / 255.f;
                        acc[0] += ((c >> 16) & 255) * a; acc[1] += ((c >> 8) & 255) * a; acc[2] += (c & 255) * a; acc[3] += a;
                        wsum += 1;
                    }
            } else {
                float fx = (x + 0.5f) * sx - 0.5f, fy = (y + 0.5f) * sy - 0.5f;
                int ix = int(fx < 0 ? -1 : fx), iy = int(fy < 0 ? -1 : fy);
                float ax = fx - ix, ay = fy - iy;
                for (int k = 0; k < 4; ++k) {
                    int xx = ix + (k & 1), yy = iy + (k >> 1);
                    xx = xx < 0 ? 0 : xx >= src.w ? src.w - 1 : xx;
                    yy = yy < 0 ? 0 : yy >= src.h ? src.h - 1 : yy;
                    float wt = ((k & 1) ? ax : 1 - ax) * ((k >> 1) ? ay : 1 - ay);
                    uint32_t c = src.px[size_t(yy) * src.w + xx];
                    float a = float(c >> 24) / 255.f;
                    acc[0] += ((c >> 16) & 255) * a * wt; acc[1] += ((c >> 8) & 255) * a * wt; acc[2] += (c & 255) * a * wt; acc[3] += a * wt;
                    wsum += wt;
                }
            }
            if (wsum <= 0) continue;
            float a = acc[3] / wsum;
            uint32_t out = 0;
            if (a > 0) {
                auto ch = [&](float v) { float r = v / wsum / a; return uint32_t(r < 0 ? 0 : r > 255 ? 255 : r + 0.5f); };
                out = ch(acc[0]) << 16 | ch(acc[1]) << 8 | ch(acc[2]);
            }
            d.px[size_t(y) * w + x] = out | uint32_t(a * 255 + 0.5f) << 24;
        }
    return d;
}

}  // namespace fo::art
