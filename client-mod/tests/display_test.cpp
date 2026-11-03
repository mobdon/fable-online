// Checks the widescreen camera maths and the bytes of the camera patch.
#include <cmath>
#include <cstdio>
#include "../src/display/camera_patch.h"

using namespace fo::display;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
static bool near(float a, float b) { return std::fabs(a - b) < 1e-4f * std::fmax(1.0f, std::fabs(b)); }

int main() {
    const float fov = 1.2f;
    // At 4:3 the patch must give exactly Fable's own picture.
    Scales o = fable_original(fov, 1024, 768), n = hor_plus(fov, 1024, 768, 1.0f);
    CHECK(near(o.xs, n.xs) && near(o.ys, n.ys));
    // Fable's own formula at 4:3: horizontal angle is fov, vertical follows from 4:3.
    CHECK(near(o.xs, 1.0f / std::tan(fov / 2)));
    CHECK(near(o.ys, o.xs * 4.0f / 3.0f));
    // Wider screens keep the 4:3 vertical view and see more to the sides.
    for (float a : {16.0f / 10.0f, 16.0f / 9.0f, 21.0f / 9.0f, 32.0f / 9.0f}) {
        Scales w = hor_plus(fov, a, 1, 1.0f);
        CHECK(near(w.ys, o.ys));
        CHECK(near(w.xs * a, w.ys));          // square pixels: no stretch
        CHECK(w.xs < o.xs);                   // wider than 4:3
        Scales f = fable_original(fov, a, 1);
        CHECK(w.ys <= f.ys && w.xs <= f.xs); // larger view both ways than Fable's own
    }
    // fov_scale widens both directions together.
    Scales s = hor_plus(fov, 16, 9, 1.1f), d = hor_plus(fov, 16, 9, 1.0f);
    CHECK(near(s.ys * 1.1f, d.ys) && near(s.xs * 1.1f, d.xs));
    // Narrower than 4:3 (portrait, square render targets) is left to Fable.
    Scales sq = hor_plus(fov, 512, 512, 1.3f), sqo = fable_original(fov, 512, 512);
    CHECK(near(sq.xs, sqo.xs) && near(sq.ys, sqo.ys));

    // Patch bytes.
    CameraSite site;
    const uint32_t cave = 0x10000000;
    auto code = build_camera_cave(cave, site, 1.0f);
    CHECK(code.size() == 0x60);
    CHECK(code[0x4F] == 0xCC);                // code ends before the constants
    auto jmp = build_camera_jump(cave, site);
    CHECK(jmp.size() == sizeof kHookBytes);
    CHECK(jmp[0] == 0xE9);
    int32_t rel = static_cast<int32_t>(jmp[1] | jmp[2] << 8 | jmp[3] << 16 | static_cast<uint32_t>(jmp[4]) << 24);
    CHECK(site.hook + 5 + static_cast<uint32_t>(rel) == cave);
    // The original je in the hook bytes lands on the automatic path.
    CHECK(site.hook + 7 + kHookBytes[6] == site.auto_fov);

    std::printf(failures ? "display tests: %d failed\n" : "display tests: all passed\n", failures);
    return failures ? 1 : 0;
}
