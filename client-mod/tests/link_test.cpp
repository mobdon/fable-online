// The mod's game connection against a real server, without Fable: the same world_client,
// frame decoding and position reporter the DLL uses, driven by a fake hero.
//
//   link_test.exe <host> <port> <ticket>
//
// Walks a hero in a line, then jumps it 200 m (the server must refuse and correct it), then
// sends a frame the server rejects so the connection drops, and checks it comes back on
// its own with the resume token from the welcome. Prints PASS or FAIL lines; exit code 0
// when everything passed. server/scripts/link-test.ts starts a server and runs this.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "log.h"
#include "net/world_client.h"
#include "world/reporter.h"
#include "world/snapshot.h"

using namespace fo;

namespace {
int g_failed = 0;
void result(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failed;
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: link_test <host> <port> <ticket>\n");
        return 2;
    }
    log_open();
    net::WorldConfig cfg;
    cfg.host = argv[1];
    cfg.port = static_cast<unsigned short>(std::atoi(argv[2]));
    cfg.ticket = argv[3];
    cfg.exeTimestamp = "link-test";
    net::world_start(cfg);

    world::PositionReporter reporter;
    float x = 50, y = 20, z = 1;
    int welcomes = 0, connects = 0, ticks = 0, corrections = 0, pongs = 0;
    uint32_t heroId = 0;
    bool jumped = false, broke = false, sawSelfMove = false;
    world::PosCorrect lastFix;
    DWORD start = GetTickCount();
    DWORD jumpAt = 0, breakAt = 0;

    while (GetTickCount() - start < 15000) {
        DWORD now = GetTickCount();
        for (net::WorldEvent& e : net::world_poll()) {
            if (e.kind == net::WorldEvent::Kind::Connected) ++connects;
            if (e.kind != net::WorldEvent::Kind::Frame) continue;
            switch (world::frame_type(e.text)) {
            case world::FrameType::Welcome: {
                world::Welcome w;
                if (world::parse_welcome(e.text, w)) {
                    ++welcomes;
                    heroId = w.playerId;
                    if (!w.resume.empty()) net::world_set_resume(w.resume);
                    reporter.reset();
                    std::printf("welcome %u as %s, reports %s, resume %s\n", w.playerId, w.name.c_str(), w.clientMove.c_str(),
                                w.resume.empty() ? "no" : "yes");
                }
                break;
            }
            case world::FrameType::Tick: {
                world::Tick t;
                if (world::parse_tick(e.text, t)) {
                    ++ticks;
                    for (auto& en : t.entities)
                        if (en.id == heroId && std::fabs(en.x - 55.0f) < 1.5f) sawSelfMove = true;
                }
                break;
            }
            case world::FrameType::PosCorrect:
                if (world::parse_pos_correct(e.text, lastFix)) {
                    ++corrections;
                    x = lastFix.x;
                    y = lastFix.y;
                    z = lastFix.z;
                    reporter.reset();
                }
                break;
            case world::FrameType::Pong: {
                double n = 0;
                if (world::parse_pong(e.text, n)) {
                    reporter.pong(now, n);
                    ++pongs;
                }
                break;
            }
            default:
                break;
            }
        }
        if (net::world_connected() && welcomes > 0) {
            if (reporter.ping_due(now)) net::world_send("{\"t\":\"ping\",\"n\":" + std::to_string(now) + "}");
            // Walk 5 m east at 5 m/s, then jump, then break the connection.
            if (!jumped && x < 55.0f) x += 0.15f;
            if (!jumped && x >= 55.0f && !jumpAt) jumpAt = now + 500;
            if (jumpAt && !jumped && now >= jumpAt) {
                jumped = true;
                x += 200.0f;
                reporter.reset();
            }
            if (jumped && corrections > 0 && !broke && !breakAt) breakAt = now + 300;
            if (breakAt && !broke && now >= breakAt) {
                broke = true;
                net::world_send("{\"t\":\"no_such_frame\"}");
            }
            if (reporter.due(now, x, y, z, 0.5f)) {
                reporter.sent(now, x, y, z, 0.5f);
                char f[160];
                std::snprintf(f, sizeof(f), "{\"t\":\"hero_pos\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"yaw\":0.5,\"area\":1}", x, y, z);
                net::world_send(f);
            }
        }
        if (broke && welcomes >= 2 && pongs >= 2 && ticks > 40) break;
        Sleep(33);
    }

    result(welcomes >= 1, "joined with the launcher ticket");
    result(ticks > 0, "tick frames arrive");
    result(sawSelfMove, "the server moved our hero with our walking reports");
    result(corrections > 0 && std::fabs(lastFix.x - 55.0f) < 1.5f, "a 200 m jump was refused and corrected back");
    result(connects >= 2 && welcomes >= 2, "after a dropped connection, came back with the resume token");
    result(pongs > 0 && reporter.rtt_ms() >= 0, "ping and pong measure the round trip");
    std::printf("round trip %d ms, %d corrections, %d ticks\n", reporter.rtt_ms(), corrections, ticks);
    net::world_stop();
    return g_failed == 0 ? 0 : 1;
}
