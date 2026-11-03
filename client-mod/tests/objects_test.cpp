// Tests for the mod's side of doors, loot and region changes (world/objects.h).
// Build and run with tests/run.sh.
#include <cstdio>
#include <string>

#include "world/objects.h"

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
using What = ObjectChange::What;

bool has(const std::vector<ObjectChange>& v, const std::string& id, What w) {
    for (const auto& c : v)
        if (c.id == id && c.what == w) return true;
    return false;
}

void test_ids_and_frames() {
    // UIDs from the level files are base + n; only the low 32 bits name the thing.
    CHECK(thing_id("LookoutPoint", 18446741874686296176ull) == "LookoutPoint:112");
    CHECK(interact_frame("LookoutPoint:112") == "{\"t\":\"interact\",\"thing\":\"LookoutPoint:112\",\"action\":\"use\"}");
    CHECK(interact_frame("A:1", "opened") == "{\"t\":\"interact\",\"thing\":\"A:1\",\"action\":\"opened\"}");
    CHECK(region_ready_frame("PicnicArea") == "{\"t\":\"region_ready\",\"region\":\"PicnicArea\"}");
    CHECK(region_ready_frame("a\"b") == "{\"t\":\"region_ready\",\"region\":\"a\\\"b\"}");
}

void test_region_change() {
    RegionChange rc;
    CHECK(parse_region_change(
        "{\"t\":\"enter_region\",\"region\":\"GreatwoodEntrance\",\"region_uid\":5,\"channel\":\"GreatwoodEntrance#1\","
        "\"x\":27.0,\"y\":148.4,\"z\":31.2,\"from\":\"LookoutPoint\",\"via\":\"exit\",\"loading\":true,"
        "\"entrance\":\"Greatwood_1:8\",\"thing\":\"LookoutPoint:112\",\"yaw\":0.31}",
        rc));
    CHECK(rc.region == "GreatwoodEntrance");
    CHECK(rc.from == "LookoutPoint");
    CHECK(rc.via == "exit");
    CHECK(rc.entrance == "Greatwood_1:8");
    CHECK(rc.thing == "LookoutPoint:112");
    CHECK(rc.loading);
    CHECK(rc.hasYaw);
    // A plain join has none of the change fields.
    CHECK(parse_region_change("{\"t\":\"enter_region\",\"region\":\"HeroGuildComplex\",\"x\":1,\"y\":2,\"z\":3}", rc));
    CHECK(!rc.loading && rc.via.empty() && !rc.hasYaw);
    CHECK(!parse_region_change("{\"t\":\"enter_region\",\"region\":\"X\",\"x\":1e9,\"y\":0,\"z\":0}", rc));
    CHECK(!parse_region_change("{\"t\":\"tick\"}", rc));
    CHECK(!parse_region_change("{\"t\":\"enter_region\",\"region\":", rc));
}

void test_objects_snapshot() {
    WorldObjects w;
    RegionChange rc;
    rc.region = "HeroGuildComplex";
    w.begin_region(rc, 0);
    CHECK(w.on_frame("{\"t\":\"tick\",\"tick\":1,\"entities\":[]}", 0) == ObjectFrame::None);
    CHECK(w.on_frame("{\"t\":\"objects\",\"region\":\"HeroGuildComplex\",\"doors\":[[\"HeroGuildComplex:20\",true]],"
                     "\"gone\":[\"HeroGuildComplex:40\"],\"opened\":[\"HeroGuildComplex:30\",\"../bad\"]}",
                     0) == ObjectFrame::Objects);
    CHECK(w.door_open("HeroGuildComplex:20", false));
    CHECK(!w.door_open("HeroGuildComplex:21", false));
    CHECK(w.door_open("HeroGuildComplex:21", true));
    CHECK(w.gone("HeroGuildComplex:40"));
    CHECK(w.opened("HeroGuildComplex:30"));
    auto ch = w.drain_changes();
    CHECK(ch.size() == 3);
    CHECK(has(ch, "HeroGuildComplex:20", What::DoorOpen));
    CHECK(has(ch, "HeroGuildComplex:40", What::Gone));
    CHECK(has(ch, "HeroGuildComplex:30", What::Opened));
    // A late snapshot for another region is ignored.
    CHECK(w.on_frame("{\"t\":\"objects\",\"region\":\"Elsewhere\",\"doors\":[],\"gone\":[],\"opened\":[]}", 0) == ObjectFrame::None);
    CHECK(w.gone("HeroGuildComplex:40"));
}

void test_updates() {
    WorldObjects w;
    CHECK(w.on_frame("{\"t\":\"object\",\"id\":\"A:20\",\"open\":true}", 0) == ObjectFrame::Object);
    CHECK(w.door_open("A:20", false));
    // The same state again is not a change.
    w.on_frame("{\"t\":\"object\",\"id\":\"A:20\",\"open\":true}", 0);
    CHECK(w.drain_changes().size() == 1);
    w.on_frame("{\"t\":\"object\",\"id\":\"A:20\",\"open\":false}", 0);
    w.on_frame("{\"t\":\"object\",\"id\":\"A:40\",\"gone\":true}", 0);
    w.on_frame("{\"t\":\"object\",\"id\":\"A:40\",\"gone\":false}", 0);
    w.on_frame("{\"t\":\"object\",\"id\":\"A:50\",\"opened\":true}", 0);
    auto ch = w.drain_changes();
    CHECK(has(ch, "A:20", What::DoorClosed));
    CHECK(has(ch, "A:40", What::Gone));
    CHECK(has(ch, "A:40", What::Back));
    CHECK(has(ch, "A:50", What::Opened));
    CHECK(!w.gone("A:40"));
}

void test_loot_and_refusals() {
    WorldObjects w;
    std::string frame;
    CHECK(w.request("A:30", "use", 1000, frame));
    CHECK(frame == interact_frame("A:30"));
    // Mashing the button while the answer is on its way sends nothing more...
    CHECK(!w.request("A:30", "use", 1200, frame));
    // ...until the answer arrives.
    CHECK(w.on_frame("{\"t\":\"loot\",\"thing\":\"A:30\",\"gold\":500,\"items\":[{\"item_id\":\"u-1\",\"def\":\"HealthPotion\"}],\"blocked\":[],\"again\":false}", 1300) ==
          ObjectFrame::Loot);
    auto loot = w.drain_loot();
    CHECK(loot.size() == 1 && loot[0].gold == 500 && loot[0].items.size() == 1 && loot[0].items[0].def == "HealthPotion" && !loot[0].again);
    CHECK(w.opened("A:30"));
    CHECK(w.request("A:30", "use", 1400, frame));
    // An unanswered request can be retried after a while.
    CHECK(w.request("A:31", "use", 2000, frame));
    CHECK(w.request("A:31", "use", 2000 + kRequestRetryMs, frame));
    // A refusal clears the wait, and "gone" hides the item.
    CHECK(w.on_frame("{\"t\":\"error\",\"error\":\"gone\",\"message\":\"Someone took it\",\"thing\":\"A:31\"}", 3100) == ObjectFrame::Refused);
    CHECK(w.gone("A:31"));
    auto refusals = w.drain_refusals();
    CHECK(refusals.size() == 1 && refusals[0].code == "gone" && refusals[0].thing == "A:31");
    CHECK(!w.request("not an id", "use", 0, frame));
}

void test_loading() {
    WorldObjects w;
    std::string frame;
    w.on_frame("{\"t\":\"object\",\"id\":\"A:20\",\"open\":true}", 0);
    RegionChange rc;
    rc.region = "PicnicArea";
    rc.loading = true;
    w.begin_region(rc, 10000);
    CHECK(w.loading());
    CHECK(!w.door_open("A:20", false));  // the old region is forgotten
    std::string f;
    CHECK(!w.request("PicnicArea:1", "use", 10000, f));  // nothing is used while loading
    CHECK(!w.finish_loading(11000, false, frame));
    CHECK(w.finish_loading(12000, true, frame));
    CHECK(frame == region_ready_frame("PicnicArea"));
    CHECK(!w.loading());
    CHECK(!w.finish_loading(13000, true, frame));  // only once
    // If the local load is never seen, the server is told anyway so the hero isn't stuck.
    w.begin_region(rc, 20000);
    CHECK(!w.finish_loading(20000 + kLoadGiveUpMs - 1, false, frame));
    CHECK(w.finish_loading(20000 + kLoadGiveUpMs, false, frame));
}

}  // namespace

int main() {
    test_ids_and_frames();
    test_region_change();
    test_objects_snapshot();
    test_updates();
    test_loot_and_refusals();
    test_loading();
    std::printf("objects: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
