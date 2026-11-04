// Tests for the parts of entity sync that have no game dependencies: frame decoding and
// the entity table. Build and run with tests/run.sh (they run on any host compiler).
#include <cmath>
#include <cstdio>
#include <string>

#include "world/entities.h"
#include "world/reporter.h"
#include "world/snapshot.h"

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
#define CLOSE(a, b) check(std::fabs((a) - (b)) < 0.01f, #a " close to " #b, __LINE__)

using namespace fo::world;

std::string tick_frame(const char* entities, int tick = 1) {
    return "{\"t\":\"tick\",\"tick\":" + std::to_string(tick) + ",\"last_input_seq\":7,\"entities\":[" + entities + "]}";
}

void test_welcome() {
    Welcome w;
    CHECK(parse_welcome("{\"t\":\"welcome\",\"player_id\":42,\"name\":\"Sparrow\",\"phase\":\"world\",\"tick_rate\":20}", w));
    CHECK(w.playerId == 42);
    CHECK(w.name == "Sparrow");
    CHECK(w.tickRate == 20);
    // A welcome with no id is not usable, and a tick frame is not a welcome.
    CHECK(!parse_welcome("{\"t\":\"welcome\",\"name\":\"x\"}", w));
    CHECK(!parse_welcome(tick_frame(""), w));
}

void test_tick_fields() {
    Tick t;
    CHECK(parse_tick(tick_frame(R"({"id":1,"name":"Mob","x":10.5,"y":-2.25,"z":3,"yaw":1.5,"anim":2,"health":87,"renown":1200},)"
                               R"({"id":1000001,"def":"CREATURE_BANDIT_GRUNT_LEVEL1","x":1,"y":2,"z":0,"yaw":0,"anim":3,"health":40})"),
                     t));
    CHECK(t.tick == 1);
    CHECK(t.lastInputSeq == 7);
    CHECK(t.entities.size() == 2);
    CHECK(t.entities[0].kind == Kind::Hero);
    CHECK(t.entities[0].name == "Mob");
    CLOSE(t.entities[0].x, 10.5f);
    CLOSE(t.entities[0].y, -2.25f);
    CHECK(t.entities[0].anim == Anim::Run);
    CHECK(t.entities[0].renown == 1200);
    CHECK(t.entities[1].kind == Kind::Creature);
    CHECK(t.entities[1].def == "CREATURE_BANDIT_GRUNT_LEVEL1");
    CHECK(t.entities[1].anim == Anim::Attack);
}

void test_tick_tolerance() {
    Tick t;
    // An entity with no position, an absurd coordinate, or an unknown field: the frame
    // still gives us the rest.
    CHECK(parse_tick(tick_frame(R"({"id":1,"y":1,"z":1},)"
                               R"({"id":2,"x":1e30,"y":0,"z":0},)"
                               R"({"id":3,"x":1,"y":2,"z":3,"future":{"a":[1,2]},"anim":99})"),
                     t));
    CHECK(t.entities.size() == 1);
    CHECK(t.entities[0].id == 3);
    CHECK(t.entities[0].anim == Anim::Idle);  // out of range falls back rather than lying

    CHECK(parse_tick(tick_frame(""), t));
    CHECK(t.entities.empty());
    CHECK(!parse_tick("{\"t\":\"tick\"", t));
    CHECK(frame_type("not json at all") == FrameType::Unknown);

    std::string region;
    float x = 0, y = 0, z = 0;
    CHECK(parse_enter_region("{\"t\":\"enter_region\",\"region\":\"Greatwood\",\"x\":1,\"y\":2,\"z\":3}", region, x, y, z));
    CHECK(region == "Greatwood");
    CLOSE(z, 3.0f);

    std::string code, message;
    CHECK(parse_error("{\"t\":\"error\",\"error\":\"bad_ticket\",\"message\":\"Hero not found\"}", code, message));
    CHECK(code == "bad_ticket");
    CHECK(message == "Hero not found");
}

void test_table_spawn_and_local() {
    EntityTable table;
    table.set_local_id(1);
    Tick t;
    parse_tick(tick_frame(R"({"id":1,"x":0,"y":0,"z":0},{"id":2,"name":"Mob","x":4,"y":0,"z":0})"), t);
    table.on_tick(t, 1000);

    // Our own hero is the player's business, not a puppet's.
    CHECK(table.size() == 1);
    auto spawned = table.drain_spawned();
    CHECK(spawned.size() == 1);
    CHECK(spawned[0].id == 2);
    CHECK(spawned[0].name == "Mob");
    CHECK(table.drain_spawned().empty());  // drained once only
}

void test_table_interpolation() {
    EntityTable table;
    Tick a, b;
    parse_tick(tick_frame(R"({"id":2,"x":0,"y":0,"z":0,"yaw":0})", 1), a);
    parse_tick(tick_frame(R"({"id":2,"x":10,"y":0,"z":0,"yaw":1})", 2), b);
    table.on_tick(a, 1000);
    table.on_tick(b, 1100);

    // Rendering runs 100 ms behind, so at 1150 the pose is halfway between the frames.
    Pose p;
    CHECK(table.pose_of(2, 1150, p));
    CLOSE(p.x, 5.0f);
    CLOSE(p.yaw, 0.5f);
    CLOSE(p.speed, 100.0f);  // 10 m in 100 ms

    // At the time of the first frame it is exactly on it, with nothing extrapolated.
    CHECK(table.pose_of(2, 1100, p));
    CLOSE(p.x, 0.0f);

    // Past the newest frame it holds still rather than sliding on through walls.
    CHECK(table.pose_of(2, 5000, p));
    CLOSE(p.x, 10.0f);
}

void test_table_removal_and_stale() {
    EntityTable table;
    Tick a, b, old;
    parse_tick(tick_frame(R"({"id":2,"x":0,"y":0,"z":0})", 5), a);
    parse_tick(tick_frame(R"({"id":3,"x":1,"y":1,"z":0})", 6), b);
    parse_tick(tick_frame(R"({"id":2,"x":99,"y":0,"z":0})", 4), old);

    table.on_tick(a, 1000);
    table.drain_spawned();
    table.on_tick(old, 1100);  // a frame from before the one we have: ignored outright
    CHECK(table.stale_frames() == 1);
    Pose p;
    CHECK(table.pose_of(2, 1200, p));
    CLOSE(p.x, 0.0f);

    // One missing frame must not despawn anything; a gap of seconds must.
    table.on_tick(b, 1200);
    CHECK(table.size() == 2);
    CHECK(table.drain_removed().empty());
    Tick c;
    parse_tick(tick_frame(R"({"id":3,"x":1,"y":1,"z":0})", 7), c);
    table.on_tick(c, 1200 + 2500);
    auto removed = table.drain_removed();
    CHECK(removed.size() == 1);
    CHECK(removed[0] == 2);
    CHECK(table.find(3) != nullptr);
    CHECK(table.find(2) == nullptr);

    // A region change takes everything with it, and reports it, so puppets are parked.
    table.clear();
    CHECK(table.size() == 0);
    CHECK(table.drain_removed().size() == 1);
}

void test_death_and_angles() {
    EntityTable table;
    Tick t;
    parse_tick(tick_frame(R"({"id":9,"def":"CREATURE_HOBBE","x":0,"y":0,"z":0,"anim":6,"health":0})"), t);
    table.on_tick(t, 1000);
    const Remote* r = table.find(9);
    CHECK(r != nullptr);
    CHECK(r->dead);
    CHECK(r->kind == Kind::Creature);

    const float pi = 3.14159265f;
    CLOSE(angle_delta(3.0f, -3.0f), 0.2832f);          // the short way round, not -6
    CLOSE(lerp_angle(-pi + 0.1f, pi - 0.1f, 0.5f), pi);  // crossing the seam
    CLOSE(lerp_angle(0.0f, 1.0f, 0.25f), 0.25f);
}

/** A frame copied from the server as it goes over the wire, parsed as the mod gets it. */
void test_real_frame() {
    const std::string frame =
        R"({"t":"tick","tick":26,"last_input_seq":0,"entities":[)"
        R"({"id":101,"kind":"hero","name":"Walker","renown":1200,"x":102.83,"y":52.83,"z":5,"yaw":2.36,"anim":2,"health":100},)"
        R"({"id":1000001,"kind":"creature","def":"CREATURE_BANDIT_GRUNT_LEVEL1","x":94,"y":52,"z":5,"yaw":0,"anim":1,"health":100}]})";
    Tick t;
    CHECK(parse_tick(frame, t));
    CHECK(t.entities.size() == 2);
    CHECK(t.entities[0].kind == Kind::Hero);
    CHECK(t.entities[0].name == "Walker");
    CLOSE(t.entities[0].x, 102.83f);
    CHECK(t.entities[1].kind == Kind::Creature);
    CHECK(t.entities[1].def == "CREATURE_BANDIT_GRUNT_LEVEL1");
    CHECK(t.entities[1].anim == Anim::Walk);
}

void test_connection_frames() {
    Welcome w;
    CHECK(parse_welcome("{\"t\":\"welcome\",\"player_id\":9,\"name\":\"Mob\",\"tick_rate\":20,"
                        "\"resume\":\"resume.abcDEF_-123\",\"resume_ms\":300000,\"client_move\":\"checked\"}", w));
    CHECK(w.resume == "resume.abcDEF_-123");
    CHECK(w.clientMove == "checked");
    // An old server says nothing about either: no resume, no reports.
    CHECK(parse_welcome("{\"t\":\"welcome\",\"player_id\":9}", w));
    CHECK(w.resume.empty());
    CHECK(w.clientMove == "off");
    CHECK(parse_welcome("{\"t\":\"welcome\",\"player_id\":9,\"client_move\":\"everything\"}", w));
    CHECK(w.clientMove == "off");

    PosCorrect c;
    CHECK(frame_type("{\"t\":\"pos_correct\",\"x\":1,\"y\":2,\"z\":3}") == FrameType::PosCorrect);
    CHECK(parse_pos_correct("{\"t\":\"pos_correct\",\"x\":32.5,\"y\":-4,\"z\":0.25,\"reason\":\"too_fast\"}", c));
    CLOSE(c.x, 32.5f);
    CLOSE(c.y, -4.0f);
    CLOSE(c.z, 0.25f);
    CHECK(c.reason == "too_fast");
    CHECK(!parse_pos_correct("{\"t\":\"pos_correct\",\"x\":1,\"y\":2}", c));
    CHECK(!parse_pos_correct("{\"t\":\"pos_correct\",\"x\":1e30,\"y\":2,\"z\":3}", c));

    double n = 0;
    CHECK(parse_pong("{\"t\":\"pong\",\"n\":123456}", n));
    CHECK(n == 123456);
    CHECK(!parse_pong("{\"t\":\"tick\",\"n\":1}", n));
}

void test_reporter() {
    PositionReporter r;
    CHECK(r.due(1000, 0, 0, 0, 0));  // first report goes straight out
    r.sent(1000, 0, 0, 0, 0);
    CHECK(!r.due(1050, 5, 0, 0, 0));  // too soon, however far
    CHECK(r.due(1100, 0.5f, 0, 0, 0));  // moving: every 100 ms
    CHECK(!r.due(1100, 0.01f, 0, 0, 0));  // standing still...
    CHECK(!r.due(1900, 0.01f, 0, 0, 0));
    CHECK(r.due(2000, 0.01f, 0, 0, 0));  // ...only a heartbeat a second
    CHECK(r.due(1200, 0, 0, 0, 0.3f));  // turning on the spot counts
    CHECK(!r.due(1200, 0, 0, 0, 6.2831853f));  // a full turn is no turn
    r.reset();
    CHECK(r.due(1010, 0, 0, 0, 0));  // after a correction, straight away

    CHECK(r.rtt_ms() == -1);
    CHECK(r.ping_due(5000));
    CHECK(!r.ping_due(6000));
    CHECK(r.ping_due(7000));
    r.pong(5080, 5000);
    CHECK(r.rtt_ms() == 80);
    r.pong(7180, 7000);
    CHECK(r.rtt_ms() == 100);  // smoothed: 80 * 0.8 + 180 * 0.2
    r.pong(9000, 1e12);  // junk is ignored
    CHECK(r.rtt_ms() == 100);
}

}  // namespace

int main() {
    std::printf("world tests\n");
    test_welcome();
    test_tick_fields();
    test_tick_tolerance();
    test_table_spawn_and_local();
    test_table_interpolation();
    test_table_removal_and_stale();
    test_death_and_angles();
    test_real_frame();
    test_connection_frames();
    test_reporter();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
