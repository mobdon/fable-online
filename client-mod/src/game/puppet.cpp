#include "puppet.h"

#include <windows.h>

#include <cstdio>

#include "../log.h"
#include "thing.h"

namespace fo::game {
namespace {
/** How often a puppet is told to drop its own actions, so Fable's AI stops steering it. */
constexpr uint32_t kClearCommandsEveryMs = 1000;
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
        default: break;
    }
    return Gait::Idle;
}

/** Names from the creature's own animation map (each creature def maps them to its own). */
const char* anim_for(Gait g) {
    switch (g) {
        case Gait::Walk: return "ST_WALK";
        case Gait::Run: return "ST_RUN";
        default: return "ST_IDLE";
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
            live_.emplace(s.id, std::move(p));
            ++stats_.reused;
            return true;
        }
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

void PuppetPool::drive(const world::Pose& pose, uint32_t nowMs) {
    auto it = live_.find(pose.id);
    if (it == live_.end()) return;
    Puppet& p = it->second;
    uintptr_t thing = p.ref.thing();
    if (!thing || !p.ref.alive()) {
        // Fable freed it: it loaded another area. Make it again once things have settled.
        ++stats_.lost;
        pending_[pose.id] = p.spawn;
        live_.erase(it);
        hold(nowMs, kHoldAfterLossMs);
        return;
    }

    // A dead creature is left where it fell with its own death animation playing; the
    // server will stop sending it when the corpse is gone.
    if (pose.anim == world::Anim::Death) return;

    write_pos(thing, Pos{pose.x, pose.y, pose.z}, true, pose.yaw);
    if (animate_) animate(p, pose, nowMs);

    // Fable's own AI keeps handing the creature somewhere to walk. Dropping its actions
    // regularly is cheaper than fighting it every frame, and the position write above is
    // what actually decides where it stands.
    if (nowMs - p.lastCommandsMs >= kClearCommandsEveryMs) {
        p.lastCommandsMs = nowMs;
        script::clear_commands(p.ref);
    }
}

void PuppetPool::animate(Puppet& p, const world::Pose& pose, uint32_t nowMs) {
    // The position write above stays the authority on where the puppet stands; this only
    // gives Fable a walk, run or idle loop to play while it is moved, through the same
    // scripted-control queue a quest's `.PlayLoopingAnim` uses.
    Gait want = gait_for(pose);
    if (want != p.wanted) {
        p.wanted = want;
        p.wantedSinceMs = nowMs;
    }
    bool settled = nowMs - p.wantedSinceMs >= kGaitSettleMs || p.gait == Gait::None;
    bool change = p.wanted != p.gait && settled;
    bool refresh = !change && p.gait != Gait::None && nowMs - p.lastAnimMs >= kGaitRefreshMs &&
                   script::scripted_queue_empty(p.ref);
    if (!change && !refresh) return;

    p.lastAnimMs = nowMs;
    Gait next = change ? p.wanted : p.gait;
    if (script::play_looping_anim(p.ref, anim_for(next), kGaitLoops, change, ++p.animSeq)) {
        if (change && stats_.animsPlayed < 20)
            fo::log("puppet: %s now %s", p.scriptName.c_str(), anim_for(next));
        p.gait = next;
        ++stats_.animsPlayed;
    } else {
        if (stats_.animsRefused++ == 0)
            fo::log("puppet: %s has no scripted control; it is moved without animations", p.scriptName.c_str());
        p.gait = next;  // do not retry every tick; the refresh check retries it slowly
    }
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
