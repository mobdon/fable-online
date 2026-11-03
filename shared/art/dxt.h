// DXT1/3/5 to 32-bit ARGB, for tools and GDI. (D3D takes DXT directly.)
#pragma once
#include "big.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fo::art {

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // 0xAARRGGBB, straight alpha
    bool empty() const { return w <= 0 || h <= 0; }
};

// kind: 1 = DXT1, 3 = DXT3, 5 = DXT5. rows should be a multiple of 4.
void decode_dxt(int kind, const uint8_t* data, size_t size, int w, int rows, std::vector<uint32_t>& out);
// Decodes a texture from a bank to ARGB, cropped to its first frame.
bool decode(const Texture& t, Image& out);
// Bilinear (growing) / box (shrinking) resize.
Image resize(const Image& src, int w, int h);

}  // namespace fo::art
