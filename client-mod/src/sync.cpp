#include "sync.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "crash_exit.h"
#include "game/hook.h"
#include "game/puppet.h"
#include "game/spawns.h"
#include "game/thing.h"
#include "game/things.h"
#include "log.h"
#include "net/world_client.h"
#include "overlay/overlay.h"
#include "world/entities.h"
#include "world/objects.h"
#include "world/reporter.h"
#include "world/snapshot.h"

namespace fo::sync {
namespace {

constexpr unsigned kTickMs = 33;        // the puppets are driven about 30 times a second
constexpr unsigned kStatusMs = 10000;
constexpr unsigned kLoadStallMs = 1500;  // no tick for this long: the game thread was busy loading
constexpr float kFarCorrection = 150.0f;
constexpr uint32_t kHoldAfterLoadMs = 3000;  // no new stand-ins this soon after an area load  // metres; a correction this far is from another map

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
    /** Doors, chests, items on the ground and the region-change handshake. */
    world::WorldObjects objects;
    /** The hero's area when a region change began, to see the local load finish. */
    uint32_t areaBeforeLoad = 0;
    /** The hero's area at the last report, to notice Fable loading another area by itself. */
    uint32_t lastArea = 0;
    /** When tick last ran; a long gap is Fable loading an area on the game thread. */
    uint32_t lastTickMs = 0;
    /** Fable stalled like an area load since the last report: tell the server with it. */
    bool loadSeen = false;
    /** The local hero object last tick; a new one means Fable rebuilt the area. */
    uintptr_t lastHero = 0;
    std::unique_ptr<game::PuppetPool> puppets;
    bool reportPos = true;
    /** What the server said it does with position reports ("off" until the welcome says). */
    std::string serverMove = "off";
    world::PositionReporter reporter;
    uint32_t corrections = 0;
    uint32_t lastCorrectionLogMs = 0;
    bool suppressSpawns = true;
    bool dev = false;
    uint32_t lastStatusMs = 0;
    uint32_t ownId = 0;
    std::string ownName;
    /** A puppet the player asked for with F8, to test spawning without a server. */
    uint32_t testId = 0;
    uint32_t testStartMs = 0;
    /** Keys sent to the server as attack and block (virtual-key codes, 0 = not sent). */
    int attackVk = VK_LBUTTON;
    int blockVk = 0;
    unsigned lastButtons = 0;
    uint32_t inputSeq = 0;
};

State* g_state = nullptr;

void on_connected() {
    if (!g_state) return;
    g_state->reporter.reset();
    g_state->objects.clear();
    g_state->table.clear();
    g_state->puppets->clear_all();
    if (g_state->suppressSpawns) game::spawns::suppress();
}

void on_disconnected(const std::string& why) {
    if (!g_state) return;
    fo::log("sync: offline (%s); dropping %u puppets", why.c_str(),
            static_cast<unsigned>(g_state->puppets->live_count()));
    g_state->table.clear();
    g_state->objects.clear();
    g_state->puppets->clear_all();
    // With no server driving them, the local generators should run the region again.
    game::spawns::restore();
}

void handle_frame(const std::string& json, uint32_t nowMs) {
    State& s = *g_state;
    if (s.objects.on_frame(json, nowMs) != world::ObjectFrame::None) return;
    switch (world::frame_type(json)) {
    case world::FrameType::Welcome: {
        world::Welcome w;
        if (!world::parse_welcome(json, w)) return;
        s.ownId = w.playerId;
        s.ownName = w.name;
        s.table.set_local_id(w.playerId);
        s.serverMove = w.clientMove;
        s.reporter.reset();
        // Kept for the next connection: a dropped link comes back without a new ticket.
        if (!w.resume.empty()) net::world_set_resume(w.resume);
        fo::log("sync: joined as %s (entity %u), server ticks %u a second, position reports %s%s", w.name.c_str(),
                w.playerId, w.tickRate, w.clientMove.c_str(), w.resume.empty() ? "" : ", can resume");
        return;
    }
    case world::FrameType::PosCorrect: {
        world::PosCorrect c;
        if (!world::parse_pos_correct(json, c)) return;
        // The server refused a move (too fast, through a wall, while dead): put the local hero
        // back where the server has it, so both agree and the next report starts from there.
        uintptr_t hero = game::local_hero();
        // A correction sent before Fable loaded another area holds the last area's coordinates.
        // Applying it would drop the hero somewhere in the new map, so it is skipped.
        if (hero && c.area && game::area_id(hero) != c.area) {
            fo::log("sync: skipped a correction meant for the last area");
            return;
        }
        // A correction from far away means the server has not followed a map change. Snapping
        // across the map would put the hero inside a wall or a gate, so leave it and let the
        // server catch up (it accepts a hero that keeps moving normally from the new spot).
        game::Pos here;
        if (hero && game::read_pos(hero, here) &&
            std::hypot(here.x - c.x, here.y - c.y) > kFarCorrection) {
            if (nowMs - s.lastCorrectionLogMs > 5000) {
                s.lastCorrectionLogMs = nowMs;
                fo::log("sync: skipped a correction %.0f m away (%s)", std::hypot(here.x - c.x, here.y - c.y), c.reason.c_str());
            }
            s.loadSeen = true;
            return;
        }
        bool moved = hero && game::write_pos(hero, game::Pos{c.x, c.y, c.z}, false, 0);
        s.reporter.reset();
        ++s.corrections;
        if (nowMs - s.lastCorrectionLogMs > 5000) {
            s.lastCorrectionLogMs = nowMs;
            fo::log("sync: server corrected the hero to %.1f, %.1f, %.1f (%s)%s; %u corrections so far", c.x, c.y, c.z,
                    c.reason.c_str(), moved ? "" : ", could not move it", s.corrections);
        }
        return;
    }
    case world::FrameType::Pong: {
        double n = 0;
        if (world::parse_pong(json, n)) s.reporter.pong(nowMs, n);
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
        world::RegionChange rc;
        if (world::parse_region_change(json, rc)) {
            if (rc.loading) {
                // The server walked us through an exit or door and holds the hero until
                // the local game has loaded the next region.
                uintptr_t hero = game::local_hero();
                s.areaBeforeLoad = hero ? game::area_id(hero) : 0;
                fo::log("sync: %s from %s via %s, arriving at %s; waiting for the local load", rc.region.c_str(),
                        rc.from.c_str(), rc.via.c_str(), rc.entrance.c_str());
            }
            s.objects.begin_region(rc, nowMs);
        }
        // No entity id carries across a region, and the hero object is rebuilt.
        s.table.clear();
        s.puppets->clear_all();
        game::forget_hero();
        s.reporter.reset();
        s.lastArea = 0;
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
    if (!net::world_connected()) return;
    if (s.reporter.ping_due(nowMs)) {
        char ping[64];
        std::snprintf(ping, sizeof(ping), "{\"t\":\"ping\",\"n\":%u}", nowMs);
        net::world_send(ping);
    }
    // While the next region loads the hero's coordinates still belong to the last one.
    if (!s.reportPos || s.serverMove == "off" || s.objects.loading()) return;
    uintptr_t hero = game::local_hero();
    if (!hero) return;
    game::Pos p;
    if (!game::read_pos(hero, p)) return;
    uint32_t area = game::area_id(hero);
    if (area && area != s.lastArea) {
        // Walking out of a town, into a house or through a Cullis Gate loads another area
        // locally. Tell the server straight away: its new coordinates belong to that area.
        if (s.lastArea) {
            fo::log("sync: Fable loaded another area (%08x), telling the server", area);
            s.reporter.reset();
        }
        s.lastArea = area;
    }
    float yaw = 0;
    bool haveYaw = game::read_yaw(hero, yaw);
    if (!s.reporter.due(nowMs, p.x, p.y, p.z, yaw)) return;
    s.reporter.sent(nowMs, p.x, p.y, p.z, yaw);
    char frame[224];
    const char* load = s.loadSeen ? ",\"load\":1" : "";
    s.loadSeen = false;
    if (haveYaw)
        std::snprintf(frame, sizeof(frame), "{\"t\":\"hero_pos\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"yaw\":%.3f,\"area\":%u%s}",
                      p.x, p.y, p.z, yaw, area, load);
    else
        std::snprintf(frame, sizeof(frame), "{\"t\":\"hero_pos\",\"x\":%.2f,\"y\":%.2f,\"z\":%.2f,\"area\":%u%s}", p.x,
                      p.y, p.z, area, load);
    net::world_send(frame);
}

/**
 * The hero's swings and guard, sent as the server's `input` buttons so the server's combat
 * decides who is hit (stand-ins are never hit by the local game: see puppet.cpp).
 *
 * Only sent while Fable has the focus and the chat box is closed, and only on a change: the
 * server fires an attack when the button is released.
 */
void report_combat(uint32_t nowMs) {
    State& s = *g_state;
    (void)nowMs;
    if (!net::world_connected() || !s.attackVk) return;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    if (fg) GetWindowThreadProcessId(fg, &pid);
    bool focused = pid == GetCurrentProcessId() && !overlay::input_blocked_now();
    auto down = [&](int vk) { return focused && vk && (GetAsyncKeyState(vk) & 0x8000) != 0; };
    unsigned buttons = (down(s.attackVk) ? 1u : 0u) | (down(s.blockVk) ? 2u : 0u);
    if (buttons == s.lastButtons) return;
    s.lastButtons = buttons;
    float yaw = 0;
    uintptr_t hero = game::local_hero();
    bool haveYaw = hero && game::read_yaw(hero, yaw);
    char frame[160];
    if (haveYaw)
        std::snprintf(frame, sizeof(frame), "{\"t\":\"input\",\"seq\":%u,\"buttons\":%u,\"camera_yaw\":%.3f}",
                      ++s.inputSeq, buttons, yaw);
    else
        std::snprintf(frame, sizeof(frame), "{\"t\":\"input\",\"seq\":%u,\"buttons\":%u}", ++s.inputSeq, buttons);
    net::world_send(frame);
}

/** Region-change handshake, and the server's door / chest / item answers. */
void sync_objects(uint32_t nowMs) {
    State& s = *g_state;
    std::string frame;
    if (s.objects.loading() && s.objects.finish_loading(nowMs, game::things::region_loaded(s.areaBeforeLoad), frame)) {
        fo::log("sync: %s loaded, telling the server", s.objects.region().region.c_str());
        net::world_send(frame);
    }
    for (const world::ObjectChange& c : s.objects.drain_changes()) game::things::apply(c);
    for (const world::Loot& l : s.objects.drain_loot()) {
        fo::log("sync: %s gave %u gold and %u items%s%s", l.thing.c_str(), l.gold, static_cast<unsigned>(l.items.size()),
                l.again ? " (already taken before)" : "", l.blocked.empty() ? "" : ", some blocked");
    }
    for (const world::Refusal& r : s.objects.drain_refusals())
        fo::log("sync: server refused %s: %s (%s)", r.thing.empty() ? "a request" : r.thing.c_str(), r.code.c_str(), r.message.c_str());
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
    crash_exit::game_alive(now);
    if (s.lastTickMs && now - s.lastTickMs > kLoadStallMs) {
        fo::log("sync: the game stalled %u ms (an area load?), telling the server", now - s.lastTickMs);
        s.loadSeen = true;
        s.reporter.reset();
        s.puppets->hold(now, kHoldAfterLoadMs);
    }
    s.lastTickMs = now;
    // No local hero means Fable is between areas or in a menu: making creatures then crashes it.
    uintptr_t heroNow = game::local_hero();
    if (!heroNow || heroNow != s.lastHero) s.puppets->hold(now, kHoldAfterLoadMs);
    s.lastHero = heroNow;

    for (net::WorldEvent& e : net::world_poll()) {
        switch (e.kind) {
        case net::WorldEvent::Kind::Connected: on_connected(); break;
        case net::WorldEvent::Kind::Disconnected: on_disconnected(e.text); break;
        case net::WorldEvent::Kind::Frame: handle_frame(e.text, now); break;
        }
    }

    report_position(now);
    report_combat(now);
    sync_objects(now);

    auto spawned = s.table.drain_spawned();
    auto removed = s.table.drain_removed();
    s.puppets->sync(spawned, removed, s.table.poses(now), now);
    drive_test_puppet(now);

    if (now - s.lastStatusMs >= kStatusMs) {
        s.lastStatusMs = now;
        uint64_t received = 0, sent = 0;
        net::world_counters(received, sent);
        const game::PuppetStats& st = s.puppets->stats();
        fo::log("sync: %s (ping %d ms, %u corrections), %u entities, puppets live %u spawned %u reused %u failed %u lost %u anims %u/%u refused, frames in %llu out %llu, stale %u",
                net::world_connected() ? "online" : "offline", s.reporter.rtt_ms(), s.corrections, static_cast<unsigned>(s.table.size()), st.live,
                st.spawned, st.reused, st.spawnFailed, st.lost, st.animsPlayed, st.animsRefused, static_cast<unsigned long long>(received),
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
    s.puppets->set_animate(env_flag("FABLE_ONLINE_PUPPET_ANIMS", true));
    // Fable's PC controls attack on the left mouse button. Block has no fixed key yet, so it
    // is only sent when a key is named (FABLE_ONLINE_BLOCK_KEY, a virtual-key code).
    if (const char* v = std::getenv("FABLE_ONLINE_ATTACK_KEY")) s.attackVk = std::atoi(v);
    if (const char* v = std::getenv("FABLE_ONLINE_BLOCK_KEY")) s.blockVk = std::atoi(v);

    fo::log("sync: stand-in heroes use %s, local spawning %s, position reports %s", heroDef.c_str(),
            s.suppressSpawns ? "suppressed in server regions" : "left alone", s.reportPos ? "on" : "off");

    crash_exit::install();
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
        fo::log("sync: %s as %s (entity %u), ping %d ms, position reports %s; %u entities, %u puppets; frames in %llu out %llu; local spawning %s",
                net::world_connected() ? "online" : "offline", s.ownName.c_str(), s.ownId, s.reporter.rtt_ms(),
                s.serverMove.c_str(),
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
