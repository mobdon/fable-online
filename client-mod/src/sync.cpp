#include "sync.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "config.h"
#include "crash_exit.h"
#include "game/hook.h"
#include "game/puppet.h"
#include "game/spawns.h"
#include "game/spirits.h"
#include "game/thing.h"
#include "game/things.h"
#include "log.h"
#include "modsettings/modsettings.h"
#include "ui/json.h"
#include "net/world_client.h"
#include "overlay/overlay.h"
#include "ui/json.h"
#include "ui/party_model.h"
#include "world/entities.h"
#include "world/health.h"
#include "world/held.h"
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
    /** Exits and chest gives Fable was stopped from doing until the server answers. */
    world::HeldActions held;
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
    /** Party members (not this player) by hero id, with their colour slot, from the last party frame. */
    std::vector<std::pair<uint32_t, int>> party;
    game::PartySpirits spirits;
    bool reportPos = true;
    /** What the server said it does with position reports ("off" until the welcome says). */
    std::string serverMove = "off";
    world::PositionReporter reporter;
    uint32_t corrections = 0;
    /** Times the server's health was written into the local hero, and when that was last logged. */
    uint32_t healthWrites = 0;
    uint32_t lastHealthLogMs = 0;
    /** Local heals (potions, food, Heal Life) waiting to be reported or answered. */
    world::HealWatch heals;
    uint32_t healSeq = 0;
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
    // Nobody will answer what is held: let Fable carry on by itself.
    g_state->held.release_all("offline");
    g_state->puppets->clear_all();
    g_state->party.clear();
    g_state->spirits.clear_all();
    overlay::clear_party();
    // With no server driving them, the local generators should run the region again.
    game::spawns::restore();
    overlay::on_world_disconnected();
}

/** Party frames: the chat and the party list show them; the member list drives the markers. */
void on_party_frame(const std::string& json) {
    State& s = *g_state;
    overlay::post_world_frame(json);
    if (json.rfind("{\"t\":\"party\"", 0) != 0) return;
    fo::json::Value m;
    if (!fo::json::parse(json, m)) return;
    s.party.clear();
    for (auto& [id, slot] : ui::party_slots(m))
        if (id != s.ownId) s.party.emplace_back(id, slot);
    s.spirits.set_members(s.party);
    fo::log("sync: party now has %u other member(s)", static_cast<unsigned>(s.party.size()));
}

/** Party requests typed in the chat go out over the world connection. */
void send_party_requests() {
    std::vector<std::string> out = overlay::take_world_requests();
    if (out.empty()) return;
    if (!net::world_connected()) {
        overlay::post_world_frame("{\"t\":\"party_error\",\"error\":\"offline\",\"message\":\"Not connected to the realm.\"}");
        return;
    }
    for (const std::string& f : out) net::world_send(f);
}

/** Where party members' stand-ins are, for the overlay's orbs and the spirits. */
void mark_party(const std::vector<world::Pose>& poses, uint32_t nowMs) {
    State& s = *g_state;
    std::vector<ui::PartyMark> marks;
    if (!s.party.empty()) {
        for (const world::Pose& p : poses) {
            if (p.kind != world::Kind::Hero) continue;
            for (auto& [id, slot] : s.party)
                if (id == p.id) marks.push_back({p.id, slot, p.x, p.y, p.z});
        }
    }
    overlay::set_party_marks(std::move(marks));
    std::map<uint32_t, std::array<float, 3>> live;
    for (const world::Pose& p : poses)
        if (p.kind == world::Kind::Hero && p.id != s.ownId) live[p.id] = {p.x, p.y, p.z};
    overlay::set_tag_positions(std::move(live));
    s.spirits.sync(poses, !s.puppets->holding(nowMs) && game::local_hero() != 0, nowMs);
}

void handle_frame(const std::string& json, uint32_t nowMs) {
    State& s = *g_state;
    // Player trading belongs to the overlay's trade window.
    if (json.find("\"t\":\"trade_") != std::string::npos && json.find("\"t\":\"trade_") < 16) {
        overlay::on_trade_frame(json);
        return;
    }
    if (s.objects.on_frame(json, nowMs) != world::ObjectFrame::None) return;
    switch (world::frame_type(json)) {
    case world::FrameType::Welcome: {
        world::Welcome w;
        if (!world::parse_welcome(json, w)) return;
        s.ownId = w.playerId;
        s.ownName = w.name;
        s.table.set_local_id(w.playerId);
        s.table.set_tick_rate(w.tickRate);
        s.serverMove = w.clientMove;
        s.reporter.reset();
        // Kept for the next connection: a dropped link comes back without a new ticket.
        if (!w.resume.empty()) net::world_set_resume(w.resume);
        // The monsters the server runs: the level files' own copies of those are left out.
        if (s.suppressSpawns) game::spawns::set_server_defs(w.spawnDefs);
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
            // The hero has been moved: an exit Fable was held at may load the region now.
            if (rc.loading) s.held.on_region_change();
        }
        // No entity id carries across a region, and the hero object is rebuilt.
        s.table.clear();
        s.puppets->clear_all();
        s.spirits.clear_all();
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
        if (json.rfind("{\"t\":\"party", 0) == 0) return on_party_frame(json);
        if (json.find("\"t\":\"heal_result\"") != std::string::npos)
            fo::log("sync: server answered a heal: %s", json.c_str());
        if (json.find("\"t\":\"dev_result\"") != std::string::npos) {
            json::Value v;
            std::string msg = json::parse(json, v) ? v.str("message") : json;
            fo::log("sync: dev: %s", msg.c_str());
            modsettings::notify(msg);
        }
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
    // Fable owns the mouse through DirectInput, which can leave the window's own button
    // state stale, so the mouse buttons are read from what Fable itself last read.
    unsigned di = overlay::mouse_buttons();
    auto down = [&](int vk) {
        if (!focused || !vk) return false;
        if (vk >= VK_LBUTTON && vk <= VK_MBUTTON) {
            int bit = vk == VK_LBUTTON ? 0 : vk == VK_RBUTTON ? 1 : 2;
            if (di & (1u << bit)) return true;
        }
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
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
    if (s.inputSeq <= 40) fo::log("sync: sent buttons %u (attack %s)", buttons, buttons & 1 ? "down" : "up");
}

/**
 * The server owns the hero's health: keep the local hero at the server's share of its max.
 * Only frames with max_health (regions running combat on the server) are applied, so a
 * region without server combat leaves Fable's own health alone.
 */
void apply_health(uint32_t nowMs) {
    State& s = *g_state;
    if (!net::world_connected() || s.objects.loading()) return s.heals.reset();
    world::EntityState own;
    if (!s.table.own_state(own) || own.maxHealth <= 0) return s.heals.reset();
    uintptr_t hero = game::local_hero();
    float hp = 0, max = 0;
    if (!hero || !game::read_health(hero, hp, max)) return s.heals.reset();
    // Fable healed the hero itself (a potion, food, Heal Life): ask the server for it. The
    // server decides what was used and heals by its own amount, or refuses.
    s.heals.observe(hp, max, nowMs);
    float share = 0;
    if (s.heals.take_report(nowMs, share)) {
        char frame[96];
        std::snprintf(frame, sizeof(frame), "{\"t\":\"heal\",\"share\":%.4f,\"key\":\"h%08x%06u\"}", share, nowMs,
                      ++s.healSeq % 1000000u);
        net::world_send(frame);
        fo::log("sync: Fable healed the hero by %.0f%% of max, asking the server", share * 100.0f);
    }
    float want = world::health_target(own.health, own.maxHealth, hp, max);
    if (want < 0) return;
    // Waiting on the server's answer to a heal: raise if it says so, never lower yet.
    if (want < hp && s.heals.holding(nowMs)) return;
    if (!game::write_health(hero, want)) return;
    s.heals.wrote(want);
    ++s.healthWrites;
    if (nowMs - s.lastHealthLogMs > 2000) {
        s.lastHealthLogMs = nowMs;
        fo::log("sync: hero health %.0f -> %.0f of %.0f (server %.0f of %.0f)", hp, want, max, own.health, own.maxHealth);
    }
}

const char* event_name(game::things::Event::Kind k) {
    switch (k) {
    case game::things::Event::Kind::Exit: return "exit";
    case game::things::Event::Kind::Give: return "give";
    case game::things::Event::Kind::DoorOpened: return "door opened";
    case game::things::Event::Kind::DoorClosed: return "door closed";
    case game::things::Event::Kind::Teleport: return "teleport to";
    case game::things::Event::Kind::TeleportBack: return "teleport";
    }
    return "?";
}

/** What the hero did with Fable's doors, chests and exits: ask the server, hold Fable's own action. */
void send_thing_events(uint32_t nowMs) {
    State& s = *g_state;
    using Kind = game::things::Event::Kind;
    for (const game::things::Event& e : game::things::drain_events()) {
        const char* action = e.kind == Kind::DoorOpened ? "open" : e.kind == Kind::DoorClosed ? "close" : "use";
        bool teleport = e.kind == Kind::Teleport || e.kind == Kind::TeleportBack;
        world::HeldKind hk = e.kind == Kind::Exit       ? world::HeldKind::Exit
                             : e.kind == Kind::Teleport ? world::HeldKind::Teleport
                             : e.kind == Kind::TeleportBack ? world::HeldKind::TeleportBack
                                                            : world::HeldKind::Give;
        // Fable can ask again while the first one waits (a search timer runs every frame).
        if (e.held && !s.held.hold(e.comp, hk, e.id, nowMs)) continue;
        std::string frame;
        bool sent = false;
        if (teleport) {
            // Cullis Gate / Guild Seal travel: the server checks the gate, combat and the law,
            // and moves the hero; enter_region then lets Fable's own jump go ahead.
            if (net::world_connected() && !s.objects.loading()) {
                frame = world::travel_frame(e.id);
                sent = true;
            }
        } else {
            sent = net::world_connected() && s.objects.request(e.id, action, nowMs, frame);
        }
        if (sent) net::world_send(frame);
        if (s.dev || e.held) fo::log("sync: %s %s%s%s", event_name(e.kind), e.id.c_str(), e.held ? ", waiting for the server" : "",
                                     sent ? "" : " (not sent)");
        // A held action that could not be asked about must not wait for an answer.
        if (e.held && !sent && !s.objects.loading()) {
            world::Refusal r;
            r.thing = e.id;
            r.code = net::world_connected() ? "busy" : "offline";
            s.held.on_refusal(r);
        }
    }
}

/** Carries out what the server decided about held exits and gives. */
void run_decisions(uint32_t nowMs) {
    State& s = *g_state;
    for (const world::Decision& d : s.held.drain(nowMs)) {
        const char* what = d.kind == world::HeldKind::Exit       ? "exit"
                           : d.kind == world::HeldKind::Give ? "give"
                                                             : "teleport";
        if (d.verdict == world::Verdict::Drop) {
            fo::log("sync: %s %s stopped by the server (%s)", what, d.id.c_str(), d.why.c_str());
            continue;
        }
        bool ok = game::things::release(d.token, d.kind);
        fo::log("sync: %s %s goes ahead (%s)%s", what, d.id.c_str(), d.why.c_str(), ok ? "" : ", but it is gone from the game");
    }
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
        s.held.on_loot(l);
    }
    for (const world::Refusal& r : s.objects.drain_refusals()) {
        fo::log("sync: server refused %s: %s (%s)", r.thing.empty() ? "a request" : r.thing.c_str(), r.code.c_str(), r.message.c_str());
        s.held.on_refusal(r);
    }
    for (const std::string& id : s.objects.drain_acks()) s.held.on_ok(id);

    // Hold Fable's exits and gives only while there is a server to answer.
    game::things::set_hold(net::world_connected() && !s.objects.loading());
    send_thing_events(nowMs);
    run_decisions(nowMs);
}

/**
 * Player trading: tell the trade window which heroes are around (for "/barter" with no name)
 * a few times a second, and send what it wants to say to the server.
 */
void sync_trade(uint32_t nowMs) {
    State& s = *g_state;
    static uint32_t lastTargetsMs = 0;
    if (nowMs - lastTargetsMs >= 250) {
        lastTargetsMs = nowMs;
        std::vector<ui::TradeTarget> targets;
        game::Pos me;
        uintptr_t hero = game::local_hero();
        bool haveMe = hero && game::read_pos(hero, me);
        for (const world::Pose& p : s.table.poses(nowMs)) {
            if (p.kind != world::Kind::Hero || p.id == s.ownId) continue;
            const world::Remote* r = s.table.find(p.id);
            if (!r || r->dead) continue;
            float d = haveMe ? std::sqrt((p.x - me.x) * (p.x - me.x) + (p.y - me.y) * (p.y - me.y) + (p.z - me.z) * (p.z - me.z)) : 1e9f;
            targets.push_back({p.id, r->name, d});
        }
        overlay::set_trade_targets(std::move(targets));
    }
    for (const std::string& f : overlay::take_world_outbox())
        if (net::world_connected()) net::world_send(f);
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

// One millisecond clock for frames, poses and puppets. GetTickCount moves in ~16 ms steps,
// which showed up as stutter once poses were sampled every frame.
uint32_t clock_ms() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<uint32_t>(static_cast<unsigned long long>(c.QuadPart) * 1000ull /
                                 static_cast<unsigned long long>(freq.QuadPart));
}

// Once a frame: move the puppets to where they should be drawn now. The 30 Hz tick does
// everything else (spawning, animations, commands); between its steps the frame rate is what
// the eye sees, so positions are kept per frame.
void frame() {
    if (!g_state) return;
    State& s = *g_state;
    uint32_t now = clock_ms();
    if (!s.lastHero || s.puppets->holding(now) || s.puppets->live_count() == 0) return;
    s.puppets->place(s.table.poses(now));
}

void tick() {
    if (!g_state) return;
    State& s = *g_state;
    uint32_t now = clock_ms();
    crash_exit::game_alive(GetTickCount());  // crash_exit compares with GetTickCount
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
    apply_health(now);
    sync_trade(now);

    send_party_requests();
    auto spawned = s.table.drain_spawned();
    auto removed = s.table.drain_removed();
    auto poses = s.table.poses(now);
    s.puppets->sync(spawned, removed, poses, now);
    mark_party(poses, now);
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
    // Fable's co-op spirit creatures over party members: never run in game yet, so opt-in.
    s.spirits.set_enabled(env_flag("FABLE_ONLINE_PARTY_SPIRITS", false));
    // Fable's PC controls attack on the left mouse button. Block has no fixed key yet, so it
    // is only sent when a key is named (FABLE_ONLINE_BLOCK_KEY, a virtual-key code).
    if (const char* v = std::getenv("FABLE_ONLINE_ATTACK_KEY")) s.attackVk = std::atoi(v);
    if (const char* v = std::getenv("FABLE_ONLINE_BLOCK_KEY")) s.blockVk = std::atoi(v);

    fo::log("sync: stand-in heroes use %s, local spawning %s, position reports %s", heroDef.c_str(),
            s.suppressSpawns ? "suppressed in server regions" : "left alone", s.reportPos ? "on" : "off");

    crash_exit::install();
    // Before Fable loads its first level, so the level's own monsters can be left out.
    if (s.suppressSpawns) game::spawns::install_level_hook();
    game::hook::install();
    game::hook::on_tick(tick, kTickMs);
    if (env_flag("FABLE_ONLINE_FRAME_PLACE", true)) game::hook::on_frame(frame);
    if (s.dev) game::hook::on_key([](int vk) { if (config().dev_hotkeys) dev_key(vk); });

    net::WorldConfig cfg;
    cfg.host = env("FABLE_ONLINE_HOST");
    cfg.port = static_cast<unsigned short>(std::atoi(env("FABLE_ONLINE_PORT", "7777").c_str()));
    cfg.ticket = env("FABLE_ONLINE_TICKET");
    cfg.exeTimestamp = env("FABLE_ONLINE_EXE_TIMESTAMP");
    if (cfg.host.empty()) {
        fo::log("sync: no server address in the environment; the mod runs offline (F8 still spawns a test puppet)");
        return;
    }
    // Doors, chests and region exits go through the server (FABLE_ONLINE_THINGS=0 leaves them local).
    if (env_flag("FABLE_ONLINE_THINGS", true)) game::things::install();
    net::world_start(cfg);
}

void stop() {
    net::world_stop();
    game::things::remove();
    if (g_state) {
        g_state->spirits.clear_all();
        g_state->puppets->clear_all();
        game::spawns::restore();
        delete g_state;
        g_state = nullptr;
    }
}

void dev_spawn(const char* def, int count) {
    if (!net::world_connected()) {
        fo::log("sync: dev spawn needs the server (offline)");
        modsettings::notify("Not connected to a server.");
        return;
    }
    char buf[160];
    std::snprintf(buf, sizeof buf, "{\"t\":\"dev_spawn\",\"def\":%s,\"count\":%d}", json::quote(def ? def : "").c_str(), count);
    fo::log("sync: asking the server to spawn %d %s", count, def && *def ? def : "monster(s)");
    net::world_send(buf);
}

void dev_key(int vk) {
    if (!g_state) return;
    State& s = *g_state;
    uint32_t now = clock_ms();
    switch (vk) {
    case VK_F6:
        if (game::spawns::suppressed()) game::spawns::restore();
        else game::spawns::suppress();
        return;
    case VK_F7: {
        uint64_t received = 0, sent = 0;
        net::world_counters(received, sent);
        float hp = 0, max = 0;
        uintptr_t hero = game::local_hero();
        world::EntityState own;
        bool haveOwn = s.table.own_state(own);
        if (hero && game::read_health(hero, hp, max))
            fo::log("sync: hero health %.1f of %.1f in Fable; server %s%.0f of %.0f; %u health writes", hp, max,
                    haveOwn ? "" : "(none yet) ", own.health, own.maxHealth, s.healthWrites);
        else
            fo::log("sync: hero health not readable (no hero yet?)");
        fo::log("sync: %s as %s (entity %u), ping %d ms, position reports %s; %u entities, %u puppets; frames in %llu out %llu; local spawning %s, %u level monsters left out",
                net::world_connected() ? "online" : "offline", s.ownName.c_str(), s.ownId, s.reporter.rtt_ms(),
                s.serverMove.c_str(),
                static_cast<unsigned>(s.table.size()), static_cast<unsigned>(s.puppets->live_count()),
                static_cast<unsigned long long>(received), static_cast<unsigned long long>(sent),
                game::spawns::suppressed() ? "off" : "on", static_cast<unsigned>(game::spawns::dropped_count()));
        return;
    }
    case VK_F8: {
        // The stand-in froze the game on its first run, so it needs an explicit opt-in.
        if (!s.testId && !env_flag("FABLE_ONLINE_STANDIN_TEST", false)) {
            fo::log("sync: F8 stand-in test is off (set FABLE_ONLINE_STANDIN_TEST=1 to allow it)");
            return;
        }
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
