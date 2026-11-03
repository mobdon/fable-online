#include "sync.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "game/hook.h"
#include "game/puppet.h"
#include "game/spawns.h"
#include "game/thing.h"
#include "log.h"
#include "net/world_client.h"
#include "world/entities.h"
#include "world/snapshot.h"

namespace fo::sync {
namespace {

constexpr unsigned kTickMs = 33;        // the puppets are driven about 30 times a second
constexpr unsigned kReportPosMs = 100;  // how often the hero's position goes to the server
constexpr unsigned kStatusMs = 10000;

std::string env(const char* name, const char* fallback = "") {
    char buf[512] = {};
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof(buf));
    return n > 0 && n < sizeof(buf) ? std::string(buf, n) : std::string(fallback);
}

bool env_flag(const char* name, bool fallback) {
    std::string v = env(name);
    if (v.empty()) return fallback;
    return v != "0" && v != "false" && v != "off";
}

struct State {
    world::EntityTable table;
    std::unique_ptr<game::PuppetPool> puppets;
    bool reportPos = true;
    bool suppressSpawns = true;
    bool dev = false;
    uint32_t lastReportMs = 0;
    uint32_t lastStatusMs = 0;
    uint32_t ownId = 0;
    std::string ownName;
    /** A puppet the player asked for with F8, to test spawning without a server. */
    uint32_t testId = 0;
    uint32_t testStartMs = 0;
};

State* g_state = nullptr;

void on_connected() {
    if (!g_state) return;
    g_state->table.clear();
    g_state->puppets->clear_all();
    if (g_state->suppressSpawns) game::spawns::suppress();
}

void on_disconnected(const std::string& why) {
    if (!g_state) return;
    fo::log("sync: offline (%s); dropping %u puppets", why.c_str(),
            static_cast<unsigned>(g_state->puppets->live_count()));
    g_state->table.clear();
    g_state->puppets->clear_all();
    // With no server driving them, the local generators should run the region again.
    game::spawns::restore();
}

void handle_frame(const std::string& json, uint32_t nowMs) {
    State& s = *g_state;
    switch (world::frame_type(json)) {
    case world::FrameType::Welcome: {
        world::Welcome w;
        if (!world::parse_welcome(json, w)) return;
        s.ownId = w.playerId;
        s.ownName = w.name;
        s.table.set_local_id(w.playerId);
        fo::log("sync: joined as %s (entity %u), server ticks %u a second", w.name.c_str(), w.playerId, w.tickRate);
        return;
    }
    case world::FrameType::Tick: {
        world::Tick t;
        if (!world::parse_tick(json, t)) return;
        s.table.on_tick(t, nowMs);
        return;
    }
    case world::FrameType::EnterRegion: {
        std::string region;
        float x = 0, y = 0, z = 0;
        if (!world::parse_enter_region(json, region, x, y, z)) return;
        fo::log("sync: region %s at %.1f, %.1f, %.1f", region.c_str(), x, y, z);
        // No entity id carries across a region, and the hero object is rebuilt.
        s.table.clear();
        s.puppets->clear_all();
        game::forget_hero();
        return;
    }
    case world::FrameType::Error: {
        std::string code, message;
        if (world::parse_error(json, code, message)) fo::log("sync: server refused us: %s (%s)", code.c_str(), message.c_str());
        return;
    }
    case world::FrameType::Unknown:
        return;
    }
}

void report_position(uint32_t nowMs) {
    State& s = *g_state;
    if (!s.reportPos || nowMs - s.lastReportMs < kReportPosMs) return;
    s.lastReportMs = nowMs;
    uintptr_t hero = game::local_hero();
    if (!hero) return;
    game::Pos p;
    if (!game::read_pos(hero, p)) return;
    char frame[192];
    std::snprintf(frame, sizeof(frame),
                  "{\"t\":\"hero_pos\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"area\":%u}", p.x, p.y, p.z,
                  game::area_id(hero));
    net::world_send(frame);
}

/** Keeps the F8 test puppet walking a circle around the hero so movement is visible. */
void drive_test_puppet(uint32_t nowMs) {
    State& s = *g_state;
    if (!s.testId) return;
    uintptr_t hero = game::local_hero();
    game::Pos h;
    if (!hero || !game::read_pos(hero, h)) return;
    float t = (nowMs - s.testStartMs) / 1000.0f;
    world::Pose pose;
    pose.id = s.testId;
    pose.kind = world::Kind::Hero;
    pose.x = h.x + 3.0f * std::cos(t * 0.8f);
    pose.y = h.y + 3.0f * std::sin(t * 0.8f);
    pose.z = h.z;
    pose.yaw = t * 0.8f;
    pose.anim = world::Anim::Walk;
    pose.health = 100;
    s.puppets->sync({}, {}, {pose}, nowMs);
}

void tick() {
    if (!g_state) return;
    State& s = *g_state;
    uint32_t now = GetTickCount();

    for (net::WorldEvent& e : net::world_poll()) {
        switch (e.kind) {
        case net::WorldEvent::Kind::Connected: on_connected(); break;
        case net::WorldEvent::Kind::Disconnected: on_disconnected(e.text); break;
        case net::WorldEvent::Kind::Frame: handle_frame(e.text, now); break;
        }
    }

    report_position(now);

    auto spawned = s.table.drain_spawned();
    auto removed = s.table.drain_removed();
    s.puppets->sync(spawned, removed, s.table.poses(now), now);
    drive_test_puppet(now);

    if (now - s.lastStatusMs >= kStatusMs) {
        s.lastStatusMs = now;
        uint64_t received = 0, sent = 0;
        net::world_counters(received, sent);
        const game::PuppetStats& st = s.puppets->stats();
        fo::log("sync: %s, %u entities, puppets live %u spawned %u reused %u failed %u lost %u, frames in %llu out %llu, stale %u",
                net::world_connected() ? "online" : "offline", static_cast<unsigned>(s.table.size()), st.live,
                st.spawned, st.reused, st.spawnFailed, st.lost, static_cast<unsigned long long>(received),
                static_cast<unsigned long long>(sent), s.table.stale_frames());
    }
}

}  // namespace

void start() {
    if (g_state) return;
    g_state = new State();
    State& s = *g_state;
    s.dev = env_flag("FABLE_ONLINE_DEV", false);
    s.reportPos = env_flag("FABLE_ONLINE_REPORT_POS", true);
    s.suppressSpawns = env_flag("FABLE_ONLINE_SUPPRESS_SPAWNS", true);
    std::string heroDef = env("FABLE_ONLINE_HERO_DEF", "CREATURE_HERO");
    s.puppets = std::make_unique<game::PuppetPool>(heroDef);

    fo::log("sync: stand-in heroes use %s, local spawning %s, position reports %s", heroDef.c_str(),
            s.suppressSpawns ? "suppressed in server regions" : "left alone", s.reportPos ? "on" : "off");

    game::hook::install();
    game::hook::on_tick(tick, kTickMs);
    if (s.dev) game::hook::on_key(dev_key);

    net::WorldConfig cfg;
    cfg.host = env("FABLE_ONLINE_HOST");
    cfg.port = static_cast<unsigned short>(std::atoi(env("FABLE_ONLINE_PORT", "7777").c_str()));
    cfg.ticket = env("FABLE_ONLINE_TICKET");
    cfg.exeTimestamp = env("FABLE_ONLINE_EXE_TIMESTAMP");
    if (cfg.host.empty()) {
        fo::log("sync: no server address in the environment; the mod runs offline (F8 still spawns a test puppet)");
        return;
    }
    net::world_start(cfg);
}

void stop() {
    net::world_stop();
    if (g_state) {
        g_state->puppets->clear_all();
        game::spawns::restore();
        delete g_state;
        g_state = nullptr;
    }
}

void dev_key(int vk) {
    if (!g_state) return;
    State& s = *g_state;
    uint32_t now = GetTickCount();
    switch (vk) {
    case VK_F6:
        if (game::spawns::suppressed()) game::spawns::restore();
        else game::spawns::suppress();
        return;
    case VK_F7: {
        uint64_t received = 0, sent = 0;
        net::world_counters(received, sent);
        fo::log("sync: %s as %s (entity %u); %u entities, %u puppets; frames in %llu out %llu; local spawning %s",
                net::world_connected() ? "online" : "offline", s.ownName.c_str(), s.ownId,
                static_cast<unsigned>(s.table.size()), static_cast<unsigned>(s.puppets->live_count()),
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(sent),
                game::spawns::suppressed() ? "off" : "on");
        return;
    }
    case VK_F8: {
        if (s.testId) {
            fo::log("sync: removing the test puppet");
            s.puppets->sync({}, {s.testId}, {}, now);
            s.testId = 0;
            return;
        }
        uintptr_t hero = game::local_hero();
        game::Pos h;
        if (!hero || !game::read_pos(hero, h)) {
            fo::log("sync: no hero yet, so no test puppet (still on the menu?)");
            return;
        }
        s.testId = 0xFFFF0001;
        s.testStartMs = now;
        world::Spawn spawn;
        spawn.id = s.testId;
        spawn.kind = world::Kind::Hero;
        spawn.x = h.x + 3.0f;
        spawn.y = h.y;
        spawn.z = h.z;
        fo::log("sync: spawning a test puppet next to the hero");
        s.puppets->sync({spawn}, {}, {}, now);
        return;
    }
    default:
        return;
    }
}

}  // namespace fo::sync
