#pragma once
// Fable's own bitmap fonts, read from data/lang/English/fonts.big in the player's install
// (never shipped). Portable C++17. Format: tools/re-notes/05-fonts.md.
#include <cstdint>
#include <string>
#include <vector>

namespace fo::assets {

struct Glyph {
    bool valid = false;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // pixel rect in the font texture (x1, y1 exclusive)
    int bearing = 0;                      // pixels from the pen to the bitmap's left edge
    int width = 0;                        // inked width
    int advance = 0;                      // pen step
};

struct Font {
    std::string face;                     // "Tahoma", "Arial"
    int height = 0;                       // cell height in pixels
    int line = 0;                         // line height in pixels
    int tex_w = 0, tex_h = 0;
    std::vector<uint32_t> argb;           // tex_w * tex_h, 0xAARRGGBB (white glyphs, alpha coverage)
    Glyph glyphs[256];
    bool empty() const { return argb.empty(); }
};

// Parses one entry from fonts.big (e.g. "ENG_TAHOMA_12"). False on anything malformed.
bool parse_font(const std::vector<uint8_t>& entry, Font& out, std::string* error = nullptr);

// Bank entry names, and the pixel size each one really is.
constexpr const char* kFontChat = "ENG_TAHOMA_12";      // Tahoma 16px: Fable's HUD text
constexpr const char* kFontSmall = "ENG_TAHOMA_10";     // Tahoma 13px
constexpr const char* kFontTitle = "ENG_ARIAL_16";      // Arial 21px: Fable's larger captions

}  // namespace fo::assets
