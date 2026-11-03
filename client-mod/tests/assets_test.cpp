// Checks the fonts.big and frontend.big readers against a real install.
// Skipped unless FABLE_DIR points at a Fable: The Lost Chapters folder. Run: tests/run.sh
#include <cstdio>
#include <cstdlib>
#include "../src/assets/fable_ui.h"

int main() {
    const char* dir = std::getenv("FABLE_DIR");
    if (!dir) { std::printf("assets: skipped (set FABLE_DIR to a Fable install)\n"); return 0; }
    auto ui = fo::assets::load_ui_utf8(dir);
    std::printf("assets: %s\n", ui.report.c_str());
    int bad = 0;
    auto check = [&](bool ok, const char* what) { if (!ok) { ++bad; std::printf("FAIL %s\n", what); } };
    check(ui.fonts_ok, "fonts load");
    check(ui.frame_ok, "frame loads");
    check(ui.chat.face == "Tahoma" && ui.chat.line == 16, "chat font is Tahoma 16");
    check(ui.title.face == "Arial" && ui.title.line == 21, "title font is Arial 21");
    for (int c = 32; c < 127; ++c) {
        const auto& g = ui.chat.glyphs[c];
        if (!g.valid || g.advance <= 0) { ++bad; std::printf("FAIL chat glyph %c\n", c); }
    }
    const auto& M = ui.chat.glyphs['M'];
    check(M.x1 - M.x0 >= M.width, "glyph rect holds its ink");
    check(ui.tl.w == 32 && ui.tl.h == 32 && ui.top.w == 8, "frame piece sizes");
    std::printf("assets: %d failed\n", bad);
    return bad ? 1 : 0;
}
