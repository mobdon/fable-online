// Tests for held exits and gives and the thing-id helpers (world/held.h).
// Build and run with tests/run.sh.
#include <cstdio>
#include <string>

#include "world/held.h"

namespace {
int g_failed = 0;
int g_checks = 0;

void check(bool ok, const char* what, int line) {
    ++g_checks;
    if (!ok) {
        ++g_failed;
        std::printf("  FAIL line %d: %s\n", line, what);
    }
}
#define CHECK(cond) check((cond), #cond, __LINE__)

using namespace fo::world;

const char* kWld =
    "MapUIDCount 72;\r\nThingManagerUIDCount 1;\r\n"
    "NewMap 1;\r\nMapX 3232;\r\nMapY 3488;\r\nLevelName \"FinalAlbion\\LookoutPoint.lev\";\r\n"
    "LevelScriptName \"LookoutPoint\";\r\nMapUID 162441;\r\nIsSea FALSE;\r\nEndMap;\r\n\r\n"
    "NewMap 5;\r\nLevelScriptName \"Greatwood_1\";\r\nMapUID 1277243;\r\nEndMap;\r\n"
    "NewMap 6;\r\nLevelScriptName \"NoUid\";\r\nEndMap;\r\n";

void test_levels_and_ids() {
    LevelNames l = parse_wld_levels(kWld);
    CHECK(l.size() == 2);
    CHECK(l[162441] == "LookoutPoint");
    CHECK(l[1277243] == "Greatwood_1");
    // Run-time UID: (MapUID << 40) | uid from the level file.
    uint64_t uid = (162441ull << 40) | 112;
    CHECK(server_thing_id(uid, l) == "LookoutPoint:112");
    // An entrance link from the level files has the same shape (Lookout Point -> Greatwood_1:8).
    CHECK(server_thing_id(0x137D3B0000000008ull, l) == "Greatwood_1:8");
    CHECK(server_thing_id((999ull << 40) | 5, l).empty());
    CHECK(runtime_uid("LookoutPoint:112", l) == uid);
    CHECK(runtime_uid("Greatwood_1:8", l) == 0x137D3B0000000008ull);
    CHECK(runtime_uid("Nowhere:8", l) == 0);
    CHECK(runtime_uid("LookoutPoint:x", l) == 0);
    CHECK(runtime_uid("LookoutPoint:99999999999", l) == 0);
}

void test_exit_moves() {
    HeldActions h;
    CHECK(h.hold(0x1000, HeldKind::Exit, "LookoutPoint:10", 0));
    CHECK(!h.hold(0x1000, HeldKind::Exit, "LookoutPoint:10", 5));  // the same exit twice
    CHECK(h.drain(100).empty());
    h.on_region_change();
    auto d = h.drain(200);
    CHECK(d.size() == 1 && d[0].token == 0x1000 && d[0].verdict == Verdict::Release && d[0].why == "moved");
    CHECK(h.size() == 0);
}

void test_exit_refusals() {
    HeldActions h;
    h.hold(1, HeldKind::Exit, "A:1", 0);
    h.hold(2, HeldKind::Exit, "A:2", 0);
    h.on_refusal(Refusal{"A:1", "demon_door_closed", "shut"});
    h.on_refusal(Refusal{"A:2", "too_far", "far"});  // could not check: go ahead
    auto d = h.drain(10);
    CHECK(d.size() == 2);
    for (auto& x : d) {
        if (x.token == 1) CHECK(x.verdict == Verdict::Drop);
        if (x.token == 2) CHECK(x.verdict == Verdict::Release && x.why == "too_far");
    }
    h.hold(3, HeldKind::Exit, "A:3", 0);
    h.on_ok("A:3");
    d = h.drain(10);
    CHECK(d.size() == 1 && d[0].verdict == Verdict::Release && d[0].why == "ok");
    // Nobody answers: the hero is never kept at the exit.
    h.hold(4, HeldKind::Exit, "A:4", 1000);
    CHECK(h.drain(1000 + kExitWaitMs - 1).empty());
    d = h.drain(1000 + kExitWaitMs);
    CHECK(d.size() == 1 && d[0].why == "timeout" && d[0].verdict == Verdict::Release);
}

void test_gives() {
    HeldActions h;
    h.hold(10, HeldKind::Give, "C:1", 0);
    h.hold(11, HeldKind::Give, "C:2", 0);
    h.hold(12, HeldKind::Give, "C:3", 0);
    h.hold(13, HeldKind::Give, "C:4", 0);
    Loot first;
    first.thing = "C:1";
    first.gold = 50;
    h.on_loot(first);
    Loot again;
    again.thing = "C:2";
    again.again = true;  // this hero already emptied it: no second give
    h.on_loot(again);
    h.on_refusal(Refusal{"C:3", "need_keys", ""});
    h.on_refusal(Refusal{"C:4", "not_here", ""});
    // A region change does not touch gives.
    h.on_region_change();
    auto d = h.drain(10);
    CHECK(d.size() == 4);
    for (auto& x : d) {
        if (x.token == 10) CHECK(x.verdict == Verdict::Release && x.why == "loot");
        if (x.token == 11) CHECK(x.verdict == Verdict::Drop && x.why == "again");
        if (x.token == 12) CHECK(x.verdict == Verdict::Drop);
        if (x.token == 13) CHECK(x.verdict == Verdict::Release);
    }
    // An ok is about exits and hooks, not gives.
    h.hold(14, HeldKind::Give, "C:5", 0);
    h.on_ok("C:5");
    CHECK(h.drain(10).empty());
    h.release_all("offline");
    d = h.drain(10);
    CHECK(d.size() == 1 && d[0].why == "offline");
    CHECK(HeldActions::refusal_blocks(HeldKind::Give, "gone"));
    CHECK(!HeldActions::refusal_blocks(HeldKind::Exit, "gone"));
    CHECK(HeldActions::refusal_blocks(HeldKind::Exit, "locked"));
}

void test_ok_frame() {
    WorldObjects o;
    CHECK(o.on_frame("{\"t\":\"ok\",\"thing\":\"LookoutPoint:10\"}", 0) == ObjectFrame::Ok);
    auto acks = o.drain_acks();
    CHECK(acks.size() == 1 && acks[0] == "LookoutPoint:10");
    CHECK(o.on_frame("{\"t\":\"ok\",\"thing\":\"bad id\"}", 0) == ObjectFrame::None);
    CHECK(o.drain_acks().empty());
}

}  // namespace

int main() {
    test_levels_and_ids();
    test_exit_moves();
    test_exit_refusals();
    test_gives();
    test_ok_frame();
    std::printf("held: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
