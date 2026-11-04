// Checks the armour recolour keeps DXT1 block modes and leaves saturated colours alone.
#include "../../shared/art/tint.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static unsigned short rd(const unsigned char* p) { return (unsigned short)(p[0] | (p[1] << 8)); }

int main() {
    fo::art::Tint t;
    // Saturated leather brown stays; mid grey turns green; dark grey turns blue.
    CHECK(fo::art::tint_pixel(0xFF8B4A1Eu, t) == 0xFF8B4A1Eu);
    unsigned g = fo::art::tint_pixel(0xFF808080u, t);
    CHECK(((g >> 8) & 255) > ((g >> 16) & 255) && ((g >> 8) & 255) > (g & 255));
    unsigned d = fo::art::tint_pixel(0xFF202020u, t);
    CHECK((d & 255) > ((d >> 16) & 255));
    CHECK((fo::art::tint_pixel(0x40808080u, t) >> 24) == 0x40);

    // The black finish darkens grey steel and keeps leather.
    fo::art::Tint k = fo::art::tint_preset("black");
    unsigned kb = fo::art::tint_pixel(0xFF808080u, k);
    CHECK(((kb >> 16) & 255) < 0x50 && ((kb >> 8) & 255) < 0x50 && (kb & 255) < 0x50);
    CHECK(fo::art::tint_pixel(0xFF8B4A1Eu, k) == 0xFF8B4A1Eu);

    // Random DXT1 blocks: mode (c0 > c1) must survive, and so must transparent pixels.
    std::srand(7);
    for (int i = 0; i < 20000; ++i) {
        unsigned char b[8];
        for (auto& x : b) x = (unsigned char)(std::rand() & 255);
        bool four = rd(b) > rd(b + 2);
        unsigned idx; std::memcpy(&idx, b + 4, 4);
        int transparent = 0;
        if (!four) for (int k = 0; k < 16; ++k) transparent += ((idx >> (2 * k)) & 3) == 3;
        fo::art::tint_dxt(1, b, 8, t);
        CHECK((rd(b) > rd(b + 2)) == four);
        if (!four) {
            unsigned n; std::memcpy(&n, b + 4, 4);
            int after = 0;
            for (int k = 0; k < 16; ++k) after += ((n >> (2 * k)) & 3) == 3;
            CHECK(after == transparent);
        }
    }
    std::printf(fails ? "tint: %d failures\n" : "tint: ok\n", fails);
    return fails ? 1 : 0;
}
