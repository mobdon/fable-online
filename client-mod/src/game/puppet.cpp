#include "puppet.h"

#include <windows.h>

#include <cstdio>

#include "../log.h"
#include "thing.h"

namespace fo::game {
namespace {
/** How often a puppet is told to drop its own actions, so Fable's AI stops steering it. */
constexpr uint32_t kClearCommandsEveryMs = 1000;
// While animations play, the AI is cleared less often: every clear also ends the animation.
constexpr uint32_t kClearCommandsAnimatedMs = 4000;
// A creature made by CreateNear is finished by the engine over its next frames; until then
// the fields the animation code reads (+0x70, +0x134) are not set. Wait well past that.
constexpr uint32_t kAnimateAfterMs = 2000;
// How long a reported hit shows as a flinch.
constexpr uint32_t kFlinchMs = 350;
// After a refused animation, how soon it is tried again.
constexpr uint32_t kRetryRefusedMs = 100;
/** The engine picks the spot within this many metres of the hero; we move it from there. */
constexpr float kSpawnRadius = 6.0f;
/** Parked puppets kept per def. Beyond this they are released. */
constexpr size_t kPoolPerDef = 8;
/** After the engine freed a puppet (an area load), wait this long before making any. */
constexpr uint32_t kHoldAfterLossMs = 5000;
/** Fewest milliseconds between two new puppets, so a crowd does not arrive in one frame. */
constexpr uint32_t kSpawnEveryMs = 250;
/**
 * A gait must be asked for this long before the puppet switches to it, so a single frame
 * of jitter at a walk/run boundary does not restart the loop (which shows as a hitch).
 */
constexpr uint32_t kGaitSettleMs = 150;
/** How often a gait loop is checked for having run out, and queued again if so. */
constexpr uint32_t kGaitRefreshMs = 500;
/** Loops per queued animation. The loop is re-queued whenever the queue runs dry. */
constexpr int kGaitLoops = 50;

Gait gait_for(const world::Pose& pose) {
    switch (pose.anim) {
        case world::Anim::Run: return Gait::Run;
        case world::Anim::Walk:
        case world::Anim::Strafe: return Gait::Walk;
        case world::Anim::Attack: return Gait::Attack;
        case world::Anim::Alert: return Gait::Alert;
        case world::Anim::Hit: return Gait::Hit;
        case world::Anim::Death: return Gait::Death;
        case world::Anim::Leap: return Gait::Leap;
        case world::Anim::Block: return Gait::Block;
        case world::Anim::Taunt: return Gait::Taunt;
        default: break;
    }
    return Gait::Idle;
}

/**
 * How a gait is played. Names come from the creature's own animation map (each creature
 * def maps them to its own sequences). `first` plays once when `once` is set, then
 * `follow` loops; `repeat` plays `first` again each time the queue runs dry.
 */
struct GaitSpec {
    const char* first;
    bool once;
    const char* follow;  // null: loop `first`
    bool repeat;
    bool immediate;  // skip kGaitSettleMs: a hit or a swing is over before it settles
};

GaitSpec spec_for(Gait g, uint32_t strikes) {
    switch (g) {
        case Gait::Walk: return {"ST_WALK", false, nullptr, false, false};
        case Gait::Run: return {"ST_RUN", false, nullptr, false, false};
        case Gait::Attack:
            return {strikes & 1 ? "ST_STRIKE_RIGHT" : "ST_STRIKE_LEFT", true, "ST_IDLE_MELEE", true, true};
        case Gait::Alert: return {"ST_IDLE_MELEE", false, nullptr, false, false};
        case Gait::Hit: return {"ST_BACK_HIT", true, "ST_IDLE_MELEE", false, true};
        case Gait::Death: return {"ST_DIE", true, "ST_DEATH_FRAME", false, true};
        case Gait::Leap: return {"ST_LEAP_IN", true, "ST_IDLE_MELEE", false, true};
        case Gait::Block: return {"ST_BLOCK_LOOP", false, nullptr, false, true};
        case Gait::Taunt: return {"ST_TAUNT", true, "ST_IDLE_MELEE", false, true};
        default: return {"ST_IDLE", false, nullptr, false, false};
    }
}
}  // namespace

void PuppetPool::hold(uint32_t nowMs, uint32_t ms) {
    uint32_t until = nowMs + ms;
    if (!holding(nowMs) || static_cast<int32_t>(until - holdUntil_) > 0) holdUntil_ = until;
}

const std::string& PuppetPool::def_for(const world::Spawn& s) const {
    return s.kind == world::Kind::Hero || s.def.empty() ? heroDef_ : s.def;
}

bool PuppetPool::spawn_for(const world::Spawn& s, uint32_t nowMs) {
    if (script::disabled()) return false;
    const std::string& def = def_for(s);

    auto pooled = pool_.find(def);
    if (pooled != pool_.end()) {
        Puppet p = std::move(pooled->second);
        pool_.erase(pooled);
        if (p.ref.alive()) {
            p.spawn = s;
            script::set_drawable(p.ref, true);
            p.hidden = false;
            p.gait = p.wanted = Gait::None;
            p.lastCommandsMs = 0;
            p.createdMs = nowMs;
            live_.emplace(s.id, std::move(p));
            ++stats_.reused;
            return true;
        }
        mark_stand_in(p.thingAddr, false);
        ++stats_.lost;  // the engine freed it while it was parked
    }

    char name[64];
    std::snprintf(name, sizeof(name), "FO_PUPPET_%u_%u", s.id, nextName_++);
    Puppet p;
    p.spawn = s;
    p.def = def;
    p.scriptName = name;
    if (!script::create_near(def.c_str(), name, kSpawnRadius, p.ref)) {
        ++stats_.spawnFailed;
        fo::log("puppet: could not create %s for entity %u", def.c_str(), s.id);
        return false;
    }
    // The server owns every hit and every death, so the local game must not fight or
    // damage a puppet: it would show results the server never agreed to.
    script::set_attackable(p.ref, false);
    script::set_damageable(p.ref, false);
    script::clear_commands(p.ref);
    p.lastCommandsMs = nowMs;
    p.createdMs = nowMs;
    p.thingAddr = p.ref.thing();
    mark_stand_in(p.thingAddr, true);
    fo::log("puppet: created %s as %s for entity %u", def.c_str(), name, s.id);
    live_.emplace(s.id, std::move(p));
    ++stats_.spawned;
    return true;
}

void PuppetPool::park(uint32_t id) {
    auto it = live_.find(id);
    if (it == live_.end()) return;
    Puppet p = std::move(it->second);
    live_.erase(it);
    if (!p.ref.alive()) {
        ++stats_.lost;
        return;
    }
    script::clear_commands(p.ref);
    script::clear_scripted(p.ref);
    script::set_drawable(p.ref, false);
    p.hidden = true;
    p.gait = p.wanted = Gait::None;
    if (pool_.count(p.def) >= kPoolPerDef) {
        script::set_free(p.ref);
        p.ref.release();
        return;
    }
    std::string def = p.def;
    pool_.emplace(std::move(def), std::move(p));
    ++stats_.parked;
}

void PuppetPool::place(const std::vector<world::Pose>& poses) {
    for (const world::Pose& pose : poses) {
        if (pose.anim == world::Anim::Death) continue;  // left where it fell
        auto it = live_.find(pose.id);
        if (it == live_.end()) continue;
        uintptr_t thing = it->second.ref.thing();  // 0 once the engine has freed it
        if (thing) write_pos(thing, Pos{pose.x, pose.y, pose.z}, true, pose.yaw);
    }
}

void PuppetPool::drive(const world::Pose& pose, uint32_t nowMs) {
    auto it = live_.find(pose.id);
    if (it == live_.end()) return;
    Puppet& p = it->second;
    uintptr_t thing = p.ref.thing();
    if (!thing || !p.ref.alive()) {
        // Fable freed it: it loaded another area. Make it again once things have settled.
        ++stats_.lost;
        mark_stand_in(p.thingAddr, false);
        pending_[pose.id] = p.spawn;
        live_.erase(it);
        hold(nowMs, kHoldAfterLossMs);
        return;
    }

    // A dead creature is left where it fell, playing its death and then lying still; the
    // server will stop sending it when the corpse is gone.
    if (pose.anim == world::Anim::Death) {
        if (animate_) animate(p, pose, nowMs);
        return;
    }

    write_pos(thing, Pos{pose.x, pose.y, pose.z}, true, pose.yaw);
    // The server's verdict on hits, visible in the log even while animations are off.
    if (p.lastHealth >= 0 && pose.health != p.lastHealth && stats_.healthLogs < 200) {
        ++stats_.healthLogs;
        fo::log("puppet: %s health %.0f -> %.0f", p.scriptName.c_str(), p.lastHealth, pose.health);
    }
    // The server only flags a flinch for some entities: any hit it reports shows one.
    if (p.lastHealth >= 0 && pose.health < p.lastHealth && pose.health > 0) p.flinchUntilMs = nowMs + kFlinchMs;
    p.lastHealth = pose.health;
    if (animate_) animate(p, pose, nowMs);

    // Fable's own AI keeps handing the creature somewhere to walk. Dropping its actions
    // regularly is cheaper than fighting it every frame, and the position write above is
    // what actually decides where it stands.
    bool animating = animate_ && p.gait != Gait::None && p.animOk;
    if (nowMs - p.lastCommandsMs >= (animating ? kClearCommandsAnimatedMs : kClearCommandsEveryMs)) {
        p.lastCommandsMs = nowMs;
        script::clear_commands(p.ref);
        if (animating) p.gait = Gait::None;  // the clear ended it: start the gait again next tick
    }
}

void PuppetPool::animate(Puppet& p, const world::Pose& pose, uint32_t nowMs) {
    // The position write stays the authority on where the puppet stands; this only gives
    // Fable an animation to play while it is moved, through the same scripted-control
    // queue a quest's `.PlayLoopingAnim` and `.PlayAnimation` use.
    if (nowMs - p.createdMs < kAnimateAfterMs) return;
    if (p.retryAtMs && static_cast<int32_t>(p.retryAtMs - nowMs) > 0) return;
    if (!p.described && stats_.described < 4) {
        p.described = true;
        ++stats_.described;
        script::describe_for_anims(p.ref, p.scriptName.c_str());
    }
    Gait want = gait_for(pose);
    if (static_cast<int32_t>(p.flinchUntilMs - nowMs) > 0 &&
        (want == Gait::Idle || want == Gait::Walk || want == Gait::Run))
        want = Gait::Hit;
    if (want != p.wanted) {
        p.wanted = want;
        p.wantedSinceMs = nowMs;
    }
    bool settled = p.gait == Gait::None || spec_for(p.wanted, 0).immediate ||
                   nowMs - p.wantedSinceMs >= kGaitSettleMs;
    bool change = p.wanted != p.gait && settled;
    bool refresh = !change && p.gait != Gait::None && nowMs - p.lastAnimMs >= kGaitRefreshMs &&
                   script::scripted_queue_empty(p.ref);
    if (!change && !refresh) return;

    p.lastAnimMs = nowMs;
    Gait next = change ? p.wanted : p.gait;
    if (next == Gait::Attack) ++p.strikes;
    GaitSpec sp = spec_for(next, p.strikes);
    uint32_t seq = ++p.animSeq;
    bool ok;
    if (change || sp.repeat) {
        // A repeating swing gets one short follow loop, so the queue runs dry and the next
        // swing starts while the server still says it is attacking.
        if (sp.once) {
            // The one-off is what matters; if its follow loop is refused, the refresh adds it.
            ok = script::play_anim_once(p.ref, sp.first, change, seq);
            if (ok) script::play_looping_anim(p.ref, sp.follow, sp.repeat ? 1 : kGaitLoops, false, ++p.animSeq);
        } else {
            ok = script::play_looping_anim(p.ref, sp.first, kGaitLoops, change, seq);
        }
    } else {
        // Ran dry: carry on with the loop, never replay a one-off (a death or a taunt).
        ok = script::play_looping_anim(p.ref, sp.follow ? sp.follow : sp.first, kGaitLoops, false, seq);
    }
    p.animOk = ok;
    if (ok) {
        if (change && stats_.animsPlayed < 40)
            fo::log("puppet: %s now %s", p.scriptName.c_str(), sp.first);
        ++stats_.animsPlayed;
        p.gait = next;
        p.retryAtMs = 0;
        return;
    }
    // A refusal is usually brief (Fable has the creature in a stagger or similar mode), so
    // the gait is left unplayed and tried again shortly, rather than given up on.
    if (stats_.animsRefused++ < 30)
        fo::log("puppet: %s refused %s (%s); trying again", p.scriptName.c_str(), sp.first,
                script::last_anim_refusal());
    p.retryAtMs = nowMs + kRetryRefusedMs;
}

void PuppetPool::sync(const std::vector<world::Spawn>& spawned, const std::vector<uint32_t>& removed,
                      const std::vector<world::Pose>& poses, uint32_t nowMs) {
    for (uint32_t id : removed) {
        pending_.erase(id);
        park(id);
    }
    for (const world::Spawn& s : spawned)
        if (!live_.count(s.id)) pending_[s.id] = s;
    for (const world::Pose& p : poses) drive(p, nowMs);
    // One new puppet at a time, and none while held.
    if (!pending_.empty() && !holding(nowMs) && nowMs - lastSpawnMs_ >= kSpawnEveryMs) {
        auto it = pending_.begin();
        world::Spawn s = it->second;
        pending_.erase(it);
        lastSpawnMs_ = nowMs;
        if (!live_.count(s.id)) spawn_for(s, nowMs);
    }
    stats_.live = static_cast<uint32_t>(live_.size());
}

void PuppetPool::clear_all() {
    pending_.clear();
    std::vector<uint32_t> ids;
    ids.reserve(live_.size());
    for (const auto& [id, p] : live_) ids.push_back(id);
    for (uint32_t id : ids) park(id);
    for (auto& [def, p] : pool_) {
        if (p.ref.alive()) script::set_free(p.ref);
        p.ref.release();
    }
    pool_.clear();
    stats_.live = 0;
}

}  // namespace fo::game
