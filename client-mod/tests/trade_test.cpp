// Tests for the portable trade window model. Run: client-mod/tests/run.sh
#include <cstdio>
#include <string>
#include "../src/ui/chat_model.h"
#include "../src/ui/json.h"
#include "../src/ui/trade_model.h"

using namespace fo;
static int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (!(cond)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define EQ(a, b) do { ++checks; auto _a = (a); auto _b = (b); if (!(_a == _b)) { ++failures; std::printf("FAIL %s:%d  %s\n   got: %s\n  want: %s\n", __FILE__, __LINE__, #a, std::string(_a).c_str(), std::string(_b).c_str()); } } while (0)

static json::Value parse(const std::string& s) { json::Value v; CHECK(json::parse(s, v)); return v; }
static void frame(ui::TradeModel& t, const std::string& s, double now) { CHECK(t.on_frame(parse(s), now)); }
static std::string last(ui::TradeModel& t) { auto o = t.take_outbox(); return o.empty() ? "" : o.back(); }

static const char* kOpen =
    R"({"t":"trade_state","trade":"T1","with":7,"name":"Hal","version":0,"settle_ms":2000,)"
    R"("mine":{"gold":0,"items":[]},"theirs":{"gold":0,"items":[]},"accepted":{"me":false,"them":false},"gold":500,)"
    R"("stock":[{"item":"aaaaaaaa-1","def":"OBJECT_HEALTH_POTION","qty":5},{"item":"bbbbbbbb-2","def":"OBJECT_IRON_LONGSWORD","qty":1}]})";

static void test_labels() {
    EQ(ui::item_label("OBJECT_HEALTH_POTION"), "Health Potion");
    EQ(ui::item_label("OBJECT_SWORD_OF_AEONS"), "Sword Of Aeons");
    EQ(ui::item_label("Apple"), "Apple");
}

static void test_invites() {
    ui::TradeModel t;
    std::vector<ui::TradeTarget> near = {{7, "Hal", 4.0f}, {8, "Maze", 25.0f}, {9, "Thunder", 2.0f}};
    t.command("", near, 0);
    EQ(last(t), R"({"t":"trade_request","to":9})");     // nearest within reach
    t.command("hal", near, 0);
    EQ(last(t), R"({"t":"trade_request","to":7})");     // by name, any case
    t.command("Th", near, 0);
    EQ(last(t), R"({"t":"trade_request","to":9})");     // unique prefix
    t.command("Nobody", near, 0);
    EQ(last(t), "");
    CHECK(!t.take_notes().empty());
    t.command("", {{8, "Maze", 25.0f}}, 0);
    EQ(last(t), "");                                       // too far away

    frame(t, R"({"t":"trade_invite","from":7,"name":"Hal","expires_ms":30000})", 0);
    CHECK(t.phase() == ui::TradeModel::Phase::Invited);
    CHECK(t.wants_keys());
    t.character(U'n', 1);
    EQ(last(t), R"({"t":"trade_answer","from":7,"yes":false})");
    CHECK(t.phase() == ui::TradeModel::Phase::Idle);

    frame(t, R"({"t":"trade_invite","from":7,"name":"Hal","expires_ms":30000})", 0);
    t.key(ui::Key::Enter, 1);
    EQ(last(t), R"({"t":"trade_answer","from":7,"yes":true})");
    frame(t, kOpen, 1);
    CHECK(t.window_open());
    EQ(t.partner(), "Hal");

    ui::TradeModel u;
    frame(u, R"({"t":"trade_invite","from":7,"name":"Hal","expires_ms":30000})", 0);
    u.step(31);
    CHECK(u.phase() == ui::TradeModel::Phase::Idle);      // ran out
    CHECK(!u.on_frame(parse(R"({"t":"tick"})"), 0));      // not a trade frame
}

static void test_offer_and_accept() {
    ui::TradeModel t;
    frame(t, kOpen, 0);
    CHECK(t.pack().size() == 2);
    // Put the potions in, then take two of them back off the stack.
    t.key(ui::Key::Enter, 0);
    CHECK(t.mine().items.size() == 1);
    CHECK(t.pack().size() == 1);
    t.key(ui::Key::Tab, 0);
    CHECK(t.focus() == ui::TradeModel::Focus::Offer);
    t.key(ui::Key::Left, 0);
    t.key(ui::Key::Left, 0);
    CHECK(t.mine().items[0].qty == 3u);
    t.key(ui::Key::Right, 0);
    t.key(ui::Key::PageDown, 0);                           // capped at the stack
    CHECK(t.mine().items[0].qty == 5u);
    t.key(ui::Key::Left, 0);
    CHECK(t.pack().size() == 2);                        // one potion still in the pack
    // Gold: typed, capped at the purse.
    t.key(ui::Key::Tab, 0);
    CHECK(t.focus() == ui::TradeModel::Focus::Gold);
    for (char32_t c : std::u32string(U"1234")) t.character(c, 0.1);
    EQ(t.gold_text(), "500");
    t.key(ui::Key::Backspace, 0.1);
    EQ(t.gold_text(), "50");
    // Nothing is sent while typing; once it pauses, the whole offer goes out.
    EQ(last(t), "");
    t.step(0.2);
    EQ(last(t), "");
    t.step(1.0);
    EQ(last(t), R"({"t":"trade_offer","gold":50,"items":[{"id":"aaaaaaaa-1","qty":4}]})");
    // Accept is refused until the server's new version has come back and settled.
    t.character(U'a', 1.0);
    EQ(last(t), "");
    CHECK(!t.problem().empty());
    frame(t,
          R"({"t":"trade_state","trade":"T1","with":7,"name":"Hal","version":2,"settle_ms":2000,)"
          R"("mine":{"gold":50,"items":[{"item":"aaaaaaaa-1","def":"OBJECT_HEALTH_POTION","qty":4}]},)"
          R"("theirs":{"gold":0,"items":[{"item":"cccccccc-3","def":"OBJECT_APPLE","qty":2}]},"accepted":{"me":false,"them":false},"gold":500})",
          1.1);
    CHECK(t.theirs().items.size() == 1);
    CHECK(t.pack().size() == 2);                        // stock kept from the opening frame
    t.character(U'a', 2.0);
    EQ(last(t), "");                                       // still settling
    CHECK(t.settle_left(2.0) > 0);
    t.character(U'a', 3.2);
    EQ(last(t), R"({"t":"trade_accept","version":2})");
    frame(t,
          R"({"t":"trade_state","trade":"T1","with":7,"name":"Hal","version":2,"settle_ms":0,)"
          R"("mine":{"gold":50,"items":[]},"theirs":{"gold":0,"items":[]},"accepted":{"me":true,"them":false},"gold":500})",
          3.3);
    CHECK(t.accepted_me());
    t.character(U'a', 3.4);
    EQ(last(t), R"({"t":"trade_unaccept"})");
    // An edit clears the ticks straight away on screen too.
    t.key(ui::Key::Tab, 3.5);   // pack
    t.key(ui::Key::Enter, 3.5);
    CHECK(!t.accepted_me() && !t.accepted_them());
    // A refusal is shown in the window and the window stays.
    frame(t, R"({"t":"trade_error","error":"insufficient_gold","message":"Not enough gold"})", 4);
    EQ(t.problem(), "Not enough gold");
    CHECK(t.window_open());
    frame(t, R"({"t":"trade_done","trade":"T1","gold":450})", 5);
    CHECK(t.phase() == ui::TradeModel::Phase::Idle);
    auto notes = t.take_notes();
    CHECK(!notes.empty() && notes.back().first.find("450 gold") != std::string::npos);
}

static void test_closing() {
    ui::TradeModel t;
    frame(t, kOpen, 0);
    t.key(ui::Key::Escape, 0);
    EQ(last(t), R"({"t":"trade_cancel"})");
    CHECK(t.phase() == ui::TradeModel::Phase::Idle);
    frame(t, R"({"t":"trade_closed","trade":"T1","reason":"cancelled"})", 0);   // ours: no second note
    CHECK(t.take_notes().size() == 2);   // "Trading with Hal" and "You cancelled"

    frame(t, kOpen, 0);
    frame(t, R"({"t":"trade_closed","trade":"T1","reason":"too_far"})", 1);
    CHECK(!t.window_open());
    frame(t, kOpen, 2);
    t.on_disconnected(3);
    CHECK(!t.window_open());
    // An error while asking (not in a window) goes to the chat log.
    t.take_notes();
    frame(t, R"({"t":"trade_asked","to":7,"name":"Hal"})", 4);
    frame(t, R"({"t":"trade_declined","by":7,"name":"Hal"})", 5);
    CHECK(t.phase() == ui::TradeModel::Phase::Idle);
    CHECK(t.take_notes().size() == 2);
}

static void test_chat_command() {
    ui::ChatModel c;
    c.key(ui::Key::Enter, 0);
    for (char32_t ch : std::u32string(U"/barter Hal")) c.character(ch, 0);
    c.key(ui::Key::Enter, 0);
    auto l = c.take_local();
    CHECK(l.size() == 1);
    EQ(l[0].first, "barter");
    EQ(l[0].second, "Hal");
    CHECK(c.take_outbox().empty());   // nothing goes to the chat server
}

int main() {
    test_labels();
    test_invites();
    test_offer_and_accept();
    test_closing();
    test_chat_command();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
