#include "sync.h"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "config.h"
#include "crash_exit.h"
#include "game/gear.h"
#include "game/hero_xp.h"
#include "game/hook.h"
#include "game/menu_pages.h"
#include "game/puppet.h"
#include "game/quests.h"
#include "game/spawns.h"
#include "game/spirits.h"
#include "game/thing.h"
#include "game/script.h"
#include "game/weather.h"
#include "game/world_time.h"
#include "game/things.h"
#include "log.h"
#include "modsettings/modsettings.h"
#include "ui/json.h"
#include "net/world_client.h"
#include "overlay/overlay.h"
#include "ui/json.h"
#include "ui/party_model.h"
#include "world/entities.h"
#include "world/death.h"
#include "world/gear_report.h"
#include "world/health.h"
#include "world/held.h"
#include "world/objects.h"
#include "world/reporter.h"
#include "world/sky.h"
#include "world/snapshot.h"
#include "world/xp_sync.h"

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
    /** Finished quests not yet sent (kept while offline; the server pays each card once). */
    std::vector<game::quests::Completed> questsDone;
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
    /** The hero's equipped weapons, read from Fable and reported so the server fights with them. */
    world::GearReporter gear;
    uint32_t lastGearReadMs = 0;
    game::HeroGear lastGear;
    bool gearLogged = false;
    uint32_t healSeq = 0;
    /** The server's death, shown without Fable's own game over (world/death.h). */
    world::DeathView death;
    /** Where the hero fell; held there while down. */
    game::Pos fellAt;
    bool haveFellAt = false;
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
    /** Renown: last read from Fable, last sent to the server, and when it was last read. */
    int32_t fableRenown = -1;
    int32_t sentRenown = -1;
    std::string renownWhy;
    uint32_t lastRenownMs = 0;
    /** The realm's time of day and weather (server `sky` frames) and Fable's clock kept on it. */
    world::Sky sky;
    bool syncClock = true;
    uint32_t lastClockMs = 0;
    uint32_t lastClockLogMs = 0;
    uint32_t clockSnaps = 0;
    /** Fable's own clock speed before the mod first changed it, put back when offline. */
    float fableSpeed = -1;
    /** Fable's own script or cutscene was holding the clock last tick (logged once). */
    bool clockHeld = false;
    /** Rain and snow follow the realm's weather (FABLE_ONLINE_SYNC_WEATHER=0 leaves them alone). */
    bool syncWeather = true;
    /** The mod turned Fable's rain or snow override on, so it has to turn it off again. */
    bool rainSet = false, snowSet = false;
    /** Hero experience kept in step with the server (only when its welcome offers it). */
    world::XpSync xp;
    bool serverXp = false;
    bool gearFailLogged = false;
    bool xpOn = true;
    uint32_t lastXpMs = 0;
    uint32_t xpWrites = 0;
    /** The hero object whose experience components were last missing, so that is logged once. */
    uintptr_t xpMissingLogged = 0;
};

State* g_state = nullptr;
/** The server's renown for this hero (-1 until it answers), and Fable's own, for the overlay. */
std::atomic<int> g_serverRenown{-1};
std::atomic<int> g_fableRenown{-1};
constexpr uint32_t kRenownEveryMs = 2000;
/** When the world was last seen playable (game thread ticking, hero present, no region load). */
std::atomic<uint32_t> g_worldShownMs{0};
/** Name tags stay hidden this long after a load or a stall, so they don't flash over the fade. */
constexpr uint32_t kTagsAfterLoadMs = 1500;
std::atomic<uint32_t> g_busyUntilMs{0};

void on_connected() {
    if (!g_state) return;
    g_state->reporter.reset();
    g_state->sentRenown = -1;  // a new connection hears the hero's renown again
    g_state->gear.reset();
    g_state->objects.clear();
    g_state->table.clear();
    g_state->puppets->clear_all();
    g_state->death.reset();
    overlay::set_fade(0, "");
    g_state->xp.reset();
    g_state->serverXp = false;
    if (g_state->suppressSpawns) game::spawns::suppress();
}

void restore_clock();
void sync_weather();

void on_disconnected(const std::string& why) {
    if (!g_state) return;
    restore_clock();
    fo::log("sync: offline (%s); dropping %u puppets", why.c_str(),
            static_cast<unsigned>(g_state->puppets->live_count()));
    g_state->table.clear();
    g_state->objects.clear();
    g_state->xp.reset();
    g_state->serverXp = false;
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

// Finished quests go to the server, which pays the card's reward into the ledger. Fable's own
// numbers are sent as read: the server pays at most what the card and its boasts can pay.
void report_quests() {
    State& s = *g_state;
    for (auto& q : game::quests::take())
        if (s.questsDone.size() < 32) s.questsDone.push_back(std::move(q));
    if (s.questsDone.empty() || !net::world_connected()) return;
    for (const auto& q : s.questsDone) {
        const std::string& id = q.card.empty() ? q.script : q.card;
        char buf[256];
        std::snprintf(buf, sizeof buf, "{\"t\":\"quest_complete\",\"quest\":%s,\"gold\":%d,\"renown\":%d,\"items\":[]}",
                      json::quote(id).c_str(), q.gold > 0 ? q.gold : 0, q.renown > 0 ? q.renown : 0);
        fo::log("quests: claiming %s (%s): %d gold, %d renown", id.c_str(), q.script.c_str(), q.gold, q.renown);
        net::world_send(buf);
    }
    s.questsDone.clear();
}

void on_quest_reward(const std::string& frame) {
    json::Value v;
    if (!json::parse(frame, v)) return;
    const std::string quest = v.str("quest");
    if (v.boolean("ok")) {
        char line[160];
        std::snprintf(line, sizeof line, "Quest reward: %d gold, %d renown", static_cast<int>(v.num("gold")),
                      static_cast<int>(v.num("renown")));
        fo::log("quests: server paid %s: %s%s", quest.c_str(), line, v.boolean("trimmed") ? " (cut down to the quest's cap)" : "");
        modsettings::notify(line);
    } else {
        fo::log("quests: server refused %s (%s)", quest.c_str(), v.str("reason").c_str());
    }
}

/** The server's time of day and weather for the region the hero is in. */
void on_sky_frame(const std::string& json, uint32_t nowMs) {
    State& s = *g_state;
    world::Sky next;
    if (!world::parse_sky(json, nowMs, next)) return;
    bool weatherChanged = !s.sky.have || next.weather != s.sky.weather || next.climate != s.sky.climate;
    s.sky = next;
    s.lastClockMs = 0;   // check Fable's clock against it on the next tick
    int h = 0, m = 0;
    world::day_hm(next.dayFraction, h, m);
    if (weatherChanged)
        fo::log("sync: realm sky %02d:%02d (%s), %s %s at %.0f%% (was %s)", h, m,
                next.night ? "night" : "day", next.climate.c_str(), next.weather.c_str(), next.intensity * 100.0f,
                next.previous.c_str());
}

/**
 * Keeps Fable's clock on the realm's: runs it a little faster or slower to close a small gap,
 * snaps it on a big one (a join, a load, a staff change). Fable's own scripts and cutscenes
 * that hold or ease the clock are left to finish first, then the clock is brought back.
 */
void sync_clock(uint32_t nowMs) {
    State& s = *g_state;
    if ((!s.syncClock && !s.syncWeather) || !s.sky.have || !net::world_connected()) return;
    if (s.lastClockMs && nowMs - s.lastClockMs < 250) return;
    s.lastClockMs = nowMs;
    sync_weather();
    if (!s.syncClock) return;
    game::world_time::Clock c;
    if (!game::world_time::read(c)) return;   // menus and area loads
    if (c.transition || c.paused) {
        if (!s.clockHeld) fo::log("sync: Fable's clock is %s by the game; the realm time waits", c.paused ? "held" : "being moved");
        s.clockHeld = true;
        return;
    }
    s.clockHeld = false;
    if (s.fableSpeed < 0) {
        s.fableSpeed = c.speed;
        fo::log("sync: Fable's own day speed %.6f a second (a %.0f minute day); the realm's is %.0f minutes", c.speed,
                c.speed > 0 ? 1.0 / c.speed / 60.0 : 0.0, s.sky.dayMs / 60000.0);
    }
    double realm = world::sky_fraction(s.sky, nowMs);
    world::ClockStep st = world::clock_step(c.time, realm, s.sky.dayMs);
    if (st.snap) {
        bool ok = game::world_time::write_time(c, static_cast<float>(st.fraction));
        ++s.clockSnaps;
        int fh = 0, fm = 0, rh = 0, rm = 0;
        world::day_hm(c.time, fh, fm);
        world::day_hm(realm, rh, rm);
        fo::log("sync: Fable's clock %02d:%02d -> realm %02d:%02d%s", fh, fm, rh, rm, ok ? "" : " (write failed)");
    }
    game::world_time::write_speed(c, static_cast<float>(st.speed));
    if (s.dev && nowMs - s.lastClockLogMs > 30000) {
        s.lastClockLogMs = nowMs;
        fo::log("sync: clock game %.4f realm %.4f gap %+.4f speed %.6f, %u snaps", c.time, realm,
                world::day_delta(c.time, realm), st.speed, s.clockSnaps);
    }
}

/** One kind of precipitation: put the override where the realm wants it, or hand it back. */
void apply_precip(const char* what, float want, bool& set, bool (*write)(float), float current) {
    if (want < 0) {
        // Only an override the mod made is turned off; a quest's own rain is left alone.
        if (set && write(-1.0f)) {
            set = false;
            fo::log("sync: %s back to the area's own", what);
        }
        return;
    }
    if (set && std::fabs(current - want) < 0.005f) return;
    if (write(want)) {
        if (!set || std::fabs(current - want) >= 0.05f) fo::log("sync: %s at %.0f%%", what, want * 100.0f);
        set = true;
    }
}

/**
 * Rain and snow from the realm's weather. Fable resets the override on area loads, so this
 * checks four times a second and puts it back.
 */
void sync_weather() {
    State& s = *g_state;
    if (!s.syncWeather || !net::world_connected()) return;
    game::weather::State w;
    if (!game::weather::read(w)) return;
    world::Precip p = world::precip_for(s.sky);
    apply_precip("rain", p.rain, s.rainSet, game::weather::set_rain, w.rainOverride);
    apply_precip("snow", p.snow, s.snowSet, game::weather::set_snow, w.snowOverride);
}

/** Offline: Fable's clock runs at its own speed again (the time of day stays where it is). */
void restore_clock() {
    if (!g_state) return;
    State& s = *g_state;
    s.sky = world::Sky{};
    if (s.rainSet && game::weather::set_rain(-1.0f)) s.rainSet = false;
    if (s.snowSet && game::weather::set_snow(-1.0f)) s.snowSet = false;
    if (s.fableSpeed < 0) return;
    game::world_time::Clock c;
    if (game::world_time::read(c)) game::world_time::write_speed(c, s.fableSpeed);
    fo::log("sync: Fable's clock back on its own speed");
    s.fableSpeed = -1;
}

/** The server's experience for this hero: kept, and written into Fable by sync_xp. */
void on_xp_state(const std::string& json) {
    State& s = *g_state;
    world::XpState st;
    if (!world::parse_xp_state(json, st)) {
        fo::log("sync: xp: could not read %s", json.c_str());
        return;
    }
    s.xp.on_state(st);
    if (st.why != "kill" || s.dev)
        fo::log("sync: xp: server has general %d, strength %d, skill %d, will %d (%s%s%s)", st.s.pools[0], st.s.pools[1], st.s.pools[2],
                st.s.pools[3], st.why.c_str(), st.bought ? ", kept a purchase" : "", st.refused ? ", refused a purchase" : "");
    if (st.refused) fo::log("sync: xp: refused: %s", json.c_str());
}

/**
 * Hero experience: report what Fable changed by itself (a purchase, a quest reward) and write
 * the server's numbers back in. Runs a few times a second, never while a region loads.
 */
void sync_xp(uint32_t nowMs) {
    State& s = *g_state;
    if (!s.xpOn || !s.serverXp || !net::world_connected() || s.objects.loading()) return;
    if (nowMs - s.lastXpMs < 200) return;
    s.lastXpMs = nowMs;
    uintptr_t hero = game::local_hero();
    if (!hero) return;
    world::XpSnapshot local;
    if (!game::read_xp(hero, local)) {
        if (s.xpMissingLogged != hero) {
            s.xpMissingLogged = hero;
            fo::log("sync: xp: can't read the hero's experience yet (%s not found)", game::xp_missing(hero));
        }
        return;
    }
    std::string frame;
    if (s.xp.step(local, nowMs, frame)) {
        net::world_send(frame);
        fo::log("sync: xp: Fable has general %d, strength %d, skill %d, will %d; telling the server", local.pools[0], local.pools[1],
                local.pools[2], local.pools[3]);
    }
    world::XpSnapshot want;
    if (!s.xp.take_write(local, want)) return;
    if (!game::write_xp(hero, want)) {
        fo::log("sync: xp: could not write the server's experience into Fable");
        return;
    }
    s.xp.wrote();
    if (++s.xpWrites <= 20 || s.dev)
        fo::log("sync: xp: wrote the server's experience into Fable (general %d -> %d)", local.pools[0], want.pools[0]);
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
        // Servers that keep hero experience say so; older ones would drop the connection on xp_sync.
        s.serverXp = json.find("\"xp\":true") != std::string::npos;
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
            if (rc.respawn) s.death.respawn_at(rc.thing, rc.x, rc.y, nowMs);
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
        if (json.rfind("{\"t\":\"renown\"", 0) == 0) {
            json::Value v;
            if (json::parse(json, v) && v.boolean("ok")) {
                g_serverRenown = static_cast<int>(v.num("renown"));
                if (v.num("gained") > 0 || v.boolean("capped"))
                    fo::log("sync: renown %d in Fable, server took %.0f (now %.0f%s)", s.sentRenown, v.num("gained"),
                            v.num("renown"), v.boolean("capped") ? ", capped" : "");
            }
            return;
        }
        if (json.find("\"t\":\"gear_result\"") != std::string::npos)
            fo::log("sync: server answered the gear report: %s", json.c_str());
        if (json.rfind("{\"t\":\"quest_reward\"", 0) == 0) return on_quest_reward(json);
        if (json.rfind("{\"t\":\"sky\"", 0) == 0) return on_sky_frame(json, nowMs);
        if (json.rfind("{\"t\":\"xp_state\"", 0) == 0) return on_xp_state(json);
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

/**
 * Renown is earned in the player's own Fable (quests run there), so read the hero's total
 * every couple of seconds and tell the server when it changes. The server decides what to
 * keep (one capped import, then a daily cap) and answers with the number it shows.
 */
void report_renown(uint32_t nowMs) {
    State& s = *g_state;
    if (nowMs - s.lastRenownMs < kRenownEveryMs) return;
    s.lastRenownMs = nowMs;
    uintptr_t hero = game::local_hero();
    int32_t total = 0;
    std::string why;
    if (!hero) return;
    if (!game::read_renown(hero, total, &why)) {
        if (why != s.renownWhy) fo::log("sync: can't read the hero's renown: %s", why.c_str());
        s.renownWhy = why;
        return;
    }
    s.renownWhy.clear();
    if (total != s.fableRenown) fo::log("sync: renown in Fable is %d", total);
    s.fableRenown = total;
    g_fableRenown = total;
    // 0 is also what a hero reads while its save is still loading: nothing to send then.
    if (!net::world_connected() || s.objects.loading() || total <= 0 || total == s.sentRenown) return;
    char frame[64];
    std::snprintf(frame, sizeof(frame), "{\"t\":\"renown_report\",\"total\":%d}", total);
    net::world_send(frame);
    s.sentRenown = total;
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
 * Tells the server which weapons and clothing the hero has equipped in Fable (twice a second, sent on a
 * change), so its combat hits with the hero's own sword. The server decides whether to accept it.
 */
void report_gear(uint32_t nowMs) {
    State& s = *g_state;
    if (!net::world_connected() || nowMs - s.lastGearReadMs < 500) return;
    s.lastGearReadMs = nowMs;
    uintptr_t hero = game::local_hero();
    game::HeroGear g;
    if (!hero) return;
    if (!game::read_gear(hero, g)) {
        if (!s.gearFailLogged && *game::gear_missing()) {
            s.gearFailLogged = true;
            fo::log("sync: can't read the hero's gear (%s)", game::gear_missing());
        }
        return;
    }
    std::string worn;
    for (const std::string& a : g.armour) worn += (worn.empty() ? "" : ", ") + a;
    if (!s.gearLogged || g.melee != s.lastGear.melee || g.ranged != s.lastGear.ranged || g.armour != s.lastGear.armour ||
        g.armourKnown != s.lastGear.armourKnown) {
        fo::log("sync: Fable has the hero holding melee '%s', ranged '%s', wearing %s", g.melee.c_str(), g.ranged.c_str(),
                g.armourKnown ? (worn.empty() ? "nothing" : worn.c_str()) : "(clothing not readable)");
        s.gearLogged = true;
        s.lastGear = g;
    }
    world::GearSeen seen;
    seen.melee = g.melee;
    seen.ranged = g.ranged;
    seen.armour = g.armour;
    seen.armourKnown = g.armourKnown;
    const std::string frame = s.gear.update(seen, nowMs);
    if (!frame.empty()) net::world_send(frame);
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

/** Fable-side of the death: the fall, holding the hero, the gate travel and getting up. */
void show_death(uint32_t nowMs) {
    State& s = *g_state;
    if (!net::world_connected()) {
        if (s.death.phase() != world::DeathView::Phase::Alive) {
            s.death.reset();
            overlay::set_fade(0, "");
        }
        return;
    }
    world::EntityState own;
    bool have = s.table.own_state(own);
    world::DeathView::Step st = s.death.update(have ? own.health : 0, have ? own.maxHealth : 0, s.objects.loading(), nowMs);
    uintptr_t hero = game::local_hero();
    if (st.fell) {
        fo::log("sync: the server has the hero dead; playing the fall (no Fable game over)");
        s.haveFellAt = hero && game::read_pos(hero, s.fellAt);
        game::script::ThingRef ref;
        if (game::script::hero(ref)) {
            // Local monsters must not finish the hero off: Fable's own death ends in a reload.
            game::script::set_damageable(ref, false);
            bool fall = game::script::play_anim_once(ref, "ST_DIE", true, 1);
            bool lie = game::script::play_looping_anim(ref, "ST_DEATH_FRAME", 1000, false, 2);
            if (!fall || !lie) fo::log("sync: the fall animation was refused (fall %d, lie %d)", fall, lie);
        }
    }
    if (st.travel) {
        fo::log("sync: respawning at %s", s.death.gate().c_str());
        s.haveFellAt = false;
        // Fable's gate travel runs its own actions on the hero: take the fall off first, so
        // nothing of ours is left queued in front of them.
        game::script::ThingRef ref;
        if (game::script::hero(ref)) game::script::clear_scripted(ref);
        if (s.death.gate().empty() || !game::things::teleport_to_gate(s.death.gate()))
            fo::log("sync: could not start the local move to the gate; the server will correct the position");
    }
    if (st.rose) {
        fo::log("sync: the hero gets up%s", st.roseInPlace ? " where they fell" : " at the gate");
        s.haveFellAt = false;
        game::script::ThingRef ref;
        if (game::script::hero(ref)) {
            // After a gate move the queue belongs to Fable's own arrival: leave it alone.
            if (st.roseInPlace) game::script::clear_scripted(ref);
            game::script::set_damageable(ref, true);
        }
    }
    // The gate move has landed (the hero is at the server's spot): the region is loaded even
    // if the area id did not change, so say so now instead of after the 25 s give-up.
    if (s.death.phase() == world::DeathView::Phase::Moving && s.objects.loading() && hero) {
        game::Pos p;
        if (game::read_pos(hero, p) && std::hypot(p.x - s.death.target_x(), p.y - s.death.target_y()) <= world::kArrivedMetres) {
            std::string frame;
            if (s.objects.finish_loading(nowMs, true, frame)) {
                fo::log("sync: the respawn move landed, telling the server");
                net::world_send(frame);
            }
        }
    }
    // Down where they fell: keep them there, whatever the keyboard says.
    if (s.death.phase() == world::DeathView::Phase::Down && s.haveFellAt && hero) game::write_pos(hero, s.fellAt, false, 0);
    float fade = s.death.fade(nowMs);
    overlay::set_fade(fade, s.death.down() ? "You have fallen" : "");
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
    // Name tags: only over a world that is being played. A stall (an area load on this
    // thread), a missing or new hero, or a region load in progress hides them for a moment.
    if (s.loadSeen || !heroNow || heroNow != s.lastHero || s.objects.loading()) g_busyUntilMs = now + kTagsAfterLoadMs;
    g_worldShownMs = now;
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
    show_death(now);
    report_renown(now);
    report_gear(now);
    sync_clock(now);
    sync_xp(now);
    sync_trade(now);
    report_quests();

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

bool world_shown() {
    const uint32_t now = GetTickCount(), at = g_worldShownMs.load();
    return at && now - at < 250 && static_cast<int32_t>(now - g_busyUntilMs.load()) >= 0;
}

int own_renown() {
    int v = g_serverRenown.load();
    return v >= 0 ? v : g_fableRenown.load();
}

void start() {
    if (g_state) return;
    g_state = new State();
    State& s = *g_state;
    s.dev = env_flag("FABLE_ONLINE_DEV", false);
    s.reportPos = env_flag("FABLE_ONLINE_REPORT_POS", true);
    s.suppressSpawns = env_flag("FABLE_ONLINE_SUPPRESS_SPAWNS", true);
    // Fable's clock follows the realm's (FABLE_ONLINE_SYNC_CLOCK=0 leaves it alone).
    s.syncClock = env_flag("FABLE_ONLINE_SYNC_CLOCK", true);
    s.syncWeather = env_flag("FABLE_ONLINE_SYNC_WEATHER", true);
    // Hero experience from the server (FABLE_ONLINE_XP=0 leaves Fable's own alone).
    s.xpOn = env_flag("FABLE_ONLINE_XP", true);
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
    // Stand-in heroes must not take over the hero's menu pages (map, inventory...).
    game::menu_pages::install();
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
    // Finished quests are claimed from the server (FABLE_ONLINE_QUESTS=0 leaves them unreported).
    if (env_flag("FABLE_ONLINE_QUESTS", true)) game::quests::install();
    net::world_start(cfg);
}

void stop() {
    restore_clock();
    net::world_stop();
    game::things::remove();
    game::menu_pages::remove();
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

void dev_despawn(bool all_near) {
    if (!net::world_connected()) {
        fo::log("sync: dev despawn needs the server (offline)");
        modsettings::notify("Not connected to a server.");
        return;
    }
    fo::log("sync: asking the server to remove %s", all_near ? "the monsters near the hero" : "the nearest monster");
    net::world_send(all_near ? "{\"t\":\"dev_despawn\",\"what\":\"near\"}" : "{\"t\":\"dev_despawn\",\"what\":\"nearest\"}");
}

void dev_time(float hour) {
    if (!net::world_connected()) {
        modsettings::notify("Not connected to a server.");
        return;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "{\"t\":\"dev_time\",\"hour\":%.2f}", hour);
    fo::log("sync: asking the server to set the realm time to %.2f", hour);
    net::world_send(buf);
}

void dev_weather(const char* weather) {
    if (!net::world_connected()) {
        modsettings::notify("Not connected to a server.");
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"t\":\"dev_weather\",\"weather\":%s,\"minutes\":30}", json::quote(weather ? weather : "auto").c_str());
    fo::log("sync: asking the server for %s weather", weather ? weather : "auto");
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
        game::world_time::Clock c;
        bool haveClock = game::world_time::read(c);
        int gh = 0, gm = 0, rh = 0, rm = 0;
        world::day_hm(haveClock ? c.time : 0, gh, gm);
        world::day_hm(world::sky_fraction(s.sky, now), rh, rm);
        if (s.sky.have)
            fo::log("sync: realm time %02d:%02d, %s %s; Fable's clock %s%02d:%02d (speed %.6f%s), %u snaps", rh, rm,
                    s.sky.climate.c_str(), s.sky.weather.c_str(), haveClock ? "" : "not found, last ", gh, gm,
                    haveClock ? c.speed : 0.0f, haveClock && (c.paused || c.transition) ? ", held by the game" : "", s.clockSnaps);
        else
            fo::log("sync: no realm sky yet; Fable's clock %s %02d:%02d", haveClock ? "at" : "not found,", gh, gm);
        game::weather::State w;
        if (game::weather::read(w))
            fo::log("sync: Fable draws rain %.2f snow %.2f; overrides rain %.2f snow %.2f (-1 = the area's own)%s%s", w.rain,
                    w.snow, w.rainOverride, w.snowOverride, s.rainSet ? ", rain set by the mod" : "", s.snowSet ? ", snow set by the mod" : "");
        else
            fo::log("sync: Fable's weather renderer not found");
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
