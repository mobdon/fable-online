#pragma once
// Fable's own HUD look for the overlay: its fonts (fonts.big) and its pewter-framed panel
// pieces (frontend.big), loaded from the player's install at start-up. Nothing is shipped.
// If the files can't be read, the overlay falls back to Windows' Tahoma and plain boxes.
#include <string>
#include "../../../shared/art/dxt.h"
#include "fable_font.h"

namespace fo::assets {

struct UiArt {
    Font chat, small, title;              // Tahoma 16, Tahoma 13, Arial 21
    // Panel frame: four 32x32 corners with the pewter rivet, and 8x8 edge tiles.
    art::Image tl, tr, bl, br, top, bottom, left, right;
    uint32_t fill = 0xFF141E26;           // panel background, sampled from UI_TABLE_BACK_SPRITE_FE
    bool fonts_ok = false, frame_ok = false;
    std::string report;                   // one line for the log
};

// game_dir: the folder holding Fable.exe.
UiArt load_ui(const std::wstring& game_dir);
UiArt load_ui_utf8(const std::string& game_dir);  // tests and tools

}  // namespace fo::assets
