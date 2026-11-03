// Checks the HUD scale choice and the screen-size patch layout.
#include <cstdio>
#include "../src/display/hud_patch.h"

using namespace fo::display;
static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    CHECK(hud_scale_for(0, 1440) > 1.59f && hud_scale_for(0, 1440) < 1.61f);
    CHECK(hud_scale_for(0, 768) == 1.0f);    // small screens keep Fable's own size
    CHECK(hud_scale_for(0, 2160) == 2.4f);
    CHECK(hud_scale_for(1.25f, 1440) == 1.25f);
    CHECK(hud_scale_for(9.0f, 1440) == 4.0f);
    ScreenSizeSite site;
    auto code = build_screen_size_cave(0x10000000, site);
    CHECK(code.size() == kVirtualSizeOffset + 8);
    CHECK(code[kVirtualSizeOffset - 1] == 0xCC);   // code ends before the size slots
    auto jmp = build_screen_size_jump(0x10000000, site);
    CHECK(jmp.size() == 6 && jmp[0] == 0xE9 && jmp[5] == 0x90);
    std::printf(failures ? "hud tests: %d failed\n" : "hud tests: all passed\n", failures);
    return failures ? 1 : 0;
}
