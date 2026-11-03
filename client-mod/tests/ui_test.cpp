// Tests for the portable chat and name tag code. Run: client-mod/tests/run.sh
#include <cmath>
#include <cstdio>
#include <string>
#include "../src/ui/chat_model.h"
#include "../src/ui/json.h"
#include "../src/ui/nametags.h"

using namespace fo;
static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define EQ(a, b) do { ++checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++failures; std::printf("FAIL %s:%d  %s\n   got: %s\n  want: %s\n", __FILE__, __LINE__, #a, std::string(_a).c_str(), std::string(_b).c_str()); } } while (0)

static void type(ui::ChatModel& c, const std::u32string& s) {
    if (!c.is_open()) c.key(ui::Key::Enter, 0);
    for (char32_t ch : s) c.character(ch, 0);
    c.key(ui::Key::Enter, 0);
}
static json::Value parse(const std::string& s) { json::Value v; CHECK(json::parse(s, v)); return v; }

static void test_json() {
    json::Value v;
    CHECK(json::parse(R"({"t":"msg","n":-1.5e2,"ok":true,"a":[1,{"x":"y"}],"s":"café 😀 \"q\""})", v));
    EQ(v.str("t"), "msg");
    CHECK(v.num("n") == -150);
    CHECK(v.boolean("ok"));
    EQ(v["a"].arr[1].str("x"), "y");
    EQ(v.str("s"), "caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\"");
    CHECK(!json::parse("{\"a\":}", v));
    CHECK(!json::parse("{\"a\":1} x", v));
    CHECK(!json::parse(std::string(100, '['), v));   // depth limit
    EQ(json::quote("a\"b\\c\n\x01"), "\"a\\\"b\\\\c\\n\\u0001\"");
    EQ(json::number(1.5), "1.5");
    EQ(json::number(-0.0001), "0");
    EQ(json::number(12), "12");
}

static void test_chat_commands() {
    ui::ChatModel c;
    CHECK(!c.is_open());
    CHECK(!c.character(U'a', 0));            // closed: game keeps its keys
    CHECK(c.key(ui::Key::Enter, 0));
    CHECK(c.is_open());
    c.key(ui::Key::Escape, 0);
    CHECK(!c.is_open());

    type(c, U"hello there");
    auto out = c.take_outbox();
    CHECK(out.size() == 1);
    EQ(out[0], R"({"t":"say","ch":"say","text":"hello there"})");

    type(c, U"/1 anyone?");
    type(c, U"still world");                 // sticky channel
    out = c.take_outbox();
    CHECK(out.size() == 2);
    EQ(out[1], R"({"t":"say","ch":"world","text":"still world"})");
    EQ(c.channel_label(), "World");

    type(c, U"/w \"Mod Kay\" hi \"there\"");
    out = c.take_outbox();
    EQ(out[0], R"({"t":"whisper","to":"Mod Kay","text":"hi \"there\""})");
    EQ(c.channel_label(), "To Mod Kay");

    type(c, U"/r hi");                       // nobody whispered us yet
    CHECK(c.take_outbox().empty());

    c.on_server(parse(R"({"t":"welcome","name":"Hal","motd":"hi"})"), 0);
    c.on_server(parse(R"({"t":"msg","ch":"whisper","from":"Maze","to":"Hal","text":"psst"})"), 0);
    type(c, U"/r what");
    out = c.take_outbox();
    EQ(out[0], R"({"t":"whisper","to":"Maze","text":"what"})");

    type(c, U"/mute Pest 10 spamming trade");
    type(c, U"/kick Pest");
    type(c, U"/ignore Pest");
    out = c.take_outbox();
    EQ(out[0], R"({"t":"mod","action":"mute","name":"Pest","minutes":10,"reason":"spamming trade"})");
    EQ(out[1], R"({"t":"mod","action":"kick","name":"Pest"})");
    EQ(out[2], R"({"t":"ignore","name":"Pest","on":true})");

    CHECK(c.tags_enabled());
    type(c, U"/tags");
    CHECK(!c.tags_enabled());
    type(c, U"/nonsense");
    CHECK(c.take_outbox().empty());
}

static void test_chat_editing_and_log() {
    ui::ChatModel c;
    c.key(ui::Key::Enter, 0);
    for (char32_t ch : std::u32string(U"helo")) c.character(ch, 0);
    c.key(ui::Key::Left, 0);
    c.character(U'l', 0);
    c.key(ui::Key::Home, 0);
    c.key(ui::Key::Delete, 0);
    EQ(c.input_utf8(), "ello");
    c.character(U'é', 0);
    EQ(c.input_utf8(), "\xC3\xA9" "ello");
    for (int i = 0; i < 300; ++i) c.character(U'x', 0);
    CHECK(ui::utf8_to_u32(c.input_utf8()).size() == ui::ChatModel::kMaxInput);
    c.key(ui::Key::Escape, 0);
    type(c, U"first");
    type(c, U"second");
    c.take_outbox();
    c.key(ui::Key::Enter, 0);
    c.key(ui::Key::Up, 0);
    EQ(c.input_utf8(), "second");
    c.key(ui::Key::Up, 0);
    EQ(c.input_utf8(), "first");
    c.key(ui::Key::Down, 0);
    c.key(ui::Key::Down, 0);
    EQ(c.input_utf8(), "");
    c.key(ui::Key::Escape, 0);

    c.on_server(parse(R"({"t":"welcome","name":"Hal"})"), 0);
    c.on_server(parse(R"({"t":"msg","ch":"region","from":"Bob","text":"HEY"})"), 10);
    c.on_server(parse(R"({"t":"msg","ch":"trade","from":"Bob","text":"WTS sword"})"), 10);
    c.on_server(parse(R"({"t":"msg","ch":"whisper","from":"Hal","to":"Bob","text":"how much"})"), 10);
    auto v = c.visible(8, 10);
    CHECK(v.size() == 3);
    EQ(v[0].line->text, "[Region] Bob: HEY");
    EQ(v[1].line->text, "[Trade] Bob: WTS sword");
    EQ(v[2].line->text, "To Bob: how much");
    CHECK(v[1].line->color == ui::ChatModel::kColorTrade);
    CHECK(c.visible(8, 10 + 13.5).size() == 3 && c.visible(8, 23.5)[0].alpha < 1.0f);  // fading
    CHECK(c.visible(8, 30).empty());          // faded out while closed
    c.key(ui::Key::Enter, 30);
    CHECK(c.visible(8, 30).size() == 3);      // all back when open
}

static void test_tabs() {
    ui::ChatModel c;
    c.on_server(parse(R"({"t":"welcome","name":"Hal"})"), 0);
    c.on_server(parse(R"({"t":"msg","ch":"region","from":"Bob","text":"in Oakvale"})"), 1);
    c.on_server(parse(R"({"t":"msg","ch":"world","from":"Ann","text":"anyone?"})"), 1);
    c.on_server(parse(R"({"t":"msg","ch":"guild","from":"Ann","text":"muster"})"), 1);
    c.on_server(parse(R"({"t":"msg","ch":"whisper","from":"Ann","to":"Hal","text":"psst"})"), 1);
    c.on_server(parse(R"({"t":"sys","text":"Server restarts soon"})"), 1);
    c.key(ui::Key::Enter, 1);
    CHECK(c.tab() == ui::ChatTab::All && c.visible(20, 1).size() == 5);
    CHECK(c.unread(ui::ChatTab::Guild) && c.unread(ui::ChatTab::World) && !c.unread(ui::ChatTab::All));
    c.key(ui::Key::Tab, 1);
    CHECK(c.tab() == ui::ChatTab::Region);
    auto v = c.visible(20, 1);
    CHECK(v.size() == 2);                                   // the region line and the system line
    EQ(v[0].line->text, "[Region] Bob: in Oakvale");
    type(c, U"hello region");
    auto out = c.take_outbox();
    EQ(out[0], R"({"t":"say","ch":"region","text":"hello region"})");  // the tab picks the channel
    c.key(ui::Key::Enter, 1);
    c.key(ui::Key::Tab, 1);
    c.key(ui::Key::Tab, 1);
    CHECK(c.tab() == ui::ChatTab::Guild && !c.unread(ui::ChatTab::Guild));
    EQ(c.channel_label(), "Guild");
    c.key(ui::Key::Tab, 1);
    EQ(c.channel_label(), "To Ann");                        // Whispers answers whoever whispered last
    c.key(ui::Key::Tab, 1);
    CHECK(c.tab() == ui::ChatTab::All);
    c.key(ui::Key::BackTab, 1);
    CHECK(c.tab() == ui::ChatTab::Whispers);
}

static void test_tags() {
    ui::TagStore s;
    s.apply(parse(R"({"t":"tags","region":7,"add":[{"id":2,"name":"Maze","renown":12500,"title":"Hero","guild":"GLD","pvp":true,"x":0,"y":10,"z":0}],"upd":[],"move":[],"del":[]})"));
    CHECK(s.size() == 1);
    // Camera at origin looking down +y, z up: row-vector view matrix, then a simple perspective.
    // view: x_v = x, y_v = z, z_v = y.  proj: x/z, y/z, w = z.
    float vp[16] = {
        1, 0, 0, 0,
        0, 0, 1, 1,
        0, 1, 0, 0,
        0, 0, 0, 0,
    };
    ui::TagSettings set;
    set.head_height = 0;
    auto l = s.layout(vp, 800, 600, set);
    CHECK(l.size() == 1);
    CHECK(std::fabs(l[0].x - 400) < 0.01f && std::fabs(l[0].y - 300) < 0.01f);
    EQ(l[0].name_line, "Maze <GLD>");
    EQ(l[0].info_line, "Hero - 12,500 renown");
    CHECK((l[0].name_color & 0xFFFFFF) == 0xE0503C);   // PvP flagged: red name
    CHECK(l[0].info_color == ui::renown_color(12500));

    s.apply(parse(R"({"t":"tags","region":7,"add":[],"upd":[],"move":[{"id":2,"x":0,"y":-10,"z":0}],"del":[]})"));
    s.step(10.0f);                                     // far jump: snaps
    CHECK(s.layout(vp, 800, 600, set).empty());        // behind the camera now

    s.apply(parse(R"({"t":"tags","region":7,"add":[],"upd":[],"move":[{"id":2,"x":0,"y":40,"z":0}],"del":[]})"));
    s.step(10.0f);
    l = s.layout(vp, 800, 600, set);
    CHECK(l.size() == 1 && (l[0].name_color >> 24) < 0xFF && (l[0].name_color >> 24) > 0);  // fading with distance

    s.apply(parse(R"({"t":"tags","region":8,"add":[],"upd":[],"move":[],"del":[]})"));
    CHECK(s.size() == 0);                              // new region: old tags gone

    EQ(ui::format_renown(0), "0 renown");
    EQ(ui::format_renown(1234567), "1,234,567 renown");
    CHECK(ui::renown_color(100) != ui::renown_color(60000));
}

int main() {
    test_json();
    test_chat_commands();
    test_chat_editing_and_log();
    test_tabs();
    test_tags();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
