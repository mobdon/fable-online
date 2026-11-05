// Mod settings screen logic. Host compiler, no Windows.
#include <cstdio>
#include "../src/modsettings/settings_model.h"

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)

using namespace fo::modsettings;

static std::vector<Page> pages(int dev_rows) {
    Page a;
    a.title = "EXTRAS";
    a.rows.push_back(Row{1, "Camera shoulder", "", {"Left", "Right", "Centre"}, 0, false});
    a.rows.push_back(Row{2, "HUD size", "", {"Off", "On"}, 0, true});
    Page d;
    d.title = "DEVELOPER";
    for (int i = 0; i < dev_rows; ++i) d.rows.push_back(Row{100 + i, "Action", "", {}, 0, false});
    return {a, d};
}

static void test_keys() {
    Model m;
    m.set_pages(pages(3));
    m.resize(1280, 720, Metrics{16, 24});
    CHECK(!m.is_open());
    CHECK(m.key(Key::Enter).kind == Event::None);   // closed: ignores input
    m.open(0);
    CHECK(m.is_open() && m.page() == 0 && m.focus() == 0);
    Event e = m.key(Key::Right);
    CHECK(e.kind == Event::Changed && e.id == 1 && e.value == 1);
    e = m.key(Key::Left);
    e = m.key(Key::Left);
    CHECK(e.value == 2);                              // wraps to Centre
    m.key(Key::Up);
    CHECK(m.focus() == -1);                           // Back button
    e = m.key(Key::Enter);
    CHECK(e.kind == Event::Closed && !m.is_open());
    m.open(0);
    m.key(Key::Tab);
    CHECK(m.page() == 1 && m.focus() == 0);
    e = m.key(Key::Enter);
    CHECK(e.kind == Event::Action && e.id == 100);
    CHECK(m.key(Key::Left).kind == Event::None);      // actions have no value
    e = m.key(Key::Escape);
    CHECK(e.kind == Event::Closed);
}

static void test_mouse_and_scroll() {
    Model m;
    m.set_pages(pages(40));
    m.resize(1920, 1080, Metrics{16, 24});
    m.open(1);
    CHECK(m.visible_rows() < 40 && m.visible_rows() > 5);
    // Focus follows the keyboard past the bottom and the list scrolls with it.
    for (int i = 0; i < 30; ++i) m.key(Key::Down);
    CHECK(m.focus() == 30 && m.first_visible() + m.visible_rows() > 30 && m.first_visible() <= 30);
    Box r = m.row(m.first_visible());
    Event e = m.click(r.x + 5, r.y + r.h / 2);
    CHECK(e.kind == Event::Action && e.id == 100 + m.first_visible());
    // Tabs switch pages; the value box's left half steps back.
    Box t = m.tab(0);
    m.click(t.x + t.w / 2, t.y + t.h / 2);
    CHECK(m.page() == 0 && m.first_visible() == 0);
    Box v = m.value_box(0);
    e = m.click(v.x + 2, v.y + v.h / 2);
    CHECK(e.kind == Event::Changed && e.value == 2);
    e = m.click(v.x + v.w - 2, v.y + v.h / 2);
    CHECK(e.kind == Event::Changed && e.value == 0);
    Box b = m.back_button();
    CHECK(b.y + b.h <= m.panel().y + m.panel().h);
    e = m.click(b.x + b.w / 2, b.y + b.h / 2);
    CHECK(e.kind == Event::Closed && !m.is_open());
}

static void test_values_and_small_screens() {
    Model m;
    m.set_pages(pages(2));
    m.set_value(1, 5);                                // clamped
    CHECK(m.find(1)->value == 2);
    m.set_value(100, 1);                              // actions have no value: ignored
    CHECK(m.find(100)->value == 0);
    m.resize(640, 480, Metrics{16, 24});
    m.open(0);
    CHECK(m.panel().x >= 0 && m.panel().y >= 0 && m.panel().x + m.panel().w <= 640 && m.panel().y + m.panel().h <= 480);
    CHECK(m.visible_rows() >= 1);
}

int main() {
    test_keys();
    test_mouse_and_scroll();
    test_values_and_small_screens();
    if (g_fail) { std::printf("%d modsettings test(s) failed\n", g_fail); return 1; }
    std::printf("modsettings tests passed\n");
    return 0;
}
