// Start-up menu logic and the shared .big reader. Host compiler, no Windows.
// Set FABLE_FRONTEND_BIG to a frontend.big from your own install to also test the reader on real data.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../shared/art/big.h"
#include "../../shared/art/lzo1x.h"
#include "../src/menu/menu_model.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace fo::menu;

static void center_of(const Button& b, float& x, float& y) { x = b.x + b.w / 2; y = b.y + b.h / 2; }

static void test_layout_scales() {
    Model m;
    m.resize(1920, 1080);
    CHECK(m.scale() == 1.5f);
    CHECK(m.buttons().size() == 5);
    const Button& first = m.buttons()[0];
    CHECK(first.label == "ENTER ALBION");
    CHECK(first.x + first.w / 2 == 960.0f);  // centred
    m.resize(1600, 1200);                     // 4:3: scale by width, centre vertically
    CHECK(m.scale() == 1.25f);
    CHECK(m.sy(0) == (1200 - 720 * 1.25f) / 2);
}

static void test_keyboard_and_screens() {
    Model m;
    m.resize(1280, 720);
    CHECK(m.focus() == 0 && m.buttons()[0].lit);
    m.key(Key::Up);
    CHECK(m.focus() == 4);                    // wraps to Leave Albion
    m.key(Key::Down);
    m.key(Key::Down);
    CHECK(m.focus() == 1);
    CHECK(m.key(Key::Enter) == Action::Heroes);
    CHECK(m.screen() == Screen::Heroes);
    CHECK(m.key(Key::Escape) == Action::Back);
    CHECK(m.screen() == Screen::Title);
    CHECK(m.buttons()[m.focus()].label == "HEROES");
    m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::Realm);
    CHECK(!m.status().empty() && m.visible());
    m.key(Key::Down);
    CHECK(m.key(Key::Enter) == Action::Options);
    CHECK(!m.visible());
    CHECK(m.key(Key::Enter) == Action::None);  // hidden: ignores input
    m.show();
    CHECK(m.visible() && m.screen() == Screen::Title && m.focus() == 0);
}

static void test_mouse() {
    Model m;
    m.resize(1280, 720);
    float x, y;
    center_of(m.buttons()[1], x, y);
    m.pointer(x, y);
    CHECK(m.focus() == 1 && m.buttons()[1].lit && !m.buttons()[0].lit);
    CHECK(m.click(5, 5) == Action::None);
    // The ornament ends of the art don't count as the button.
    const Button& b = m.buttons()[0];
    CHECK(m.click(b.x + 2, b.y + b.h / 2) == Action::None);
    CHECK(m.click(x, y) == Action::Heroes);
    m.set_hero_name("Maze");
    CHECK(m.buttons()[0].label == "MAZE");
    center_of(m.buttons()[1], x, y);
    unsigned serial = m.status_serial();
    CHECK(m.click(x, y) == Action::NewHero && m.status_serial() != serial);
    center_of(m.buttons()[3], x, y);
    CHECK(m.click(x, y) == Action::EnterAlbion && !m.visible());
}

static void test_lzo() {
    // A literal-only stream: 0x11 + 3 bytes? Use the minimal form: first byte 17+n literals, then end marker.
    const uint8_t in[] = {17 + 4, 'F', 'a', 'b', 'l', 0x11, 0x00, 0x00};
    uint8_t out[16] = {};
    long n = fo::art::lzo1x_decompress(in, sizeof in, out, sizeof out);
    CHECK(n == 4 && std::memcmp(out, "Fabl", 4) == 0);
    CHECK(fo::art::lzo1x_decompress(in, sizeof in, out, 2) == -1);   // output too small
    const uint8_t bad[] = {0x40, 0xFF};                               // match before any output
    CHECK(fo::art::lzo1x_decompress(bad, sizeof bad, out, sizeof out) == -1);
}

static void test_real_bank() {
    const char* path = std::getenv("FABLE_FRONTEND_BIG");
    if (!path) { std::printf("skip: set FABLE_FRONTEND_BIG to test the reader on frontend.big\n"); return; }
    fo::art::Bank b;
    CHECK(b.open(std::string(path)));
    CHECK(b.names().size() == 394);
    int ok = 0;
    for (auto& n : b.names()) { fo::art::Texture t; ok += b.load(n, t); }
    CHECK(ok == 394);
    fo::art::Texture t;
    CHECK(b.load("FRONTEND_BG_01_SPRITE", t) && t.pixels == fo::art::PixelFormat::DXT1 && t.width == 256 && t.mip0.size() == 32768);
    CHECK(b.load("FE_BUTTON_ON", t) && t.width == 256 && t.height == 64 && t.alpha);
    CHECK(b.load("FRONTEND_BACKDROP_01", t) && t.frame_w == 640 && t.frame_h == 480);
    CHECK(!b.load("NOT_A_TEXTURE", t));
}

int main() {
    test_layout_scales();
    test_keyboard_and_screens();
    test_mouse();
    test_lzo();
    test_real_bank();
    std::printf(g_fail ? "%d menu test(s) failed\n" : "menu tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
