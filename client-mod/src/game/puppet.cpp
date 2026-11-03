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
}  // namespace

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
            script::set_drawable(p.ref, true);
            p.hidden = false;
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
    script::set_drawable(p.ref, false);
    p.hidden = true;
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
        ++stats_.lost;
        live_.erase(it);
        return;
    }

    // A dead creature is left where it fell with its own death animation playing; the
    // server will stop sending it when the corpse is gone.
    if (pose.anim == world::Anim::Death) return;

    write_pos(thing, Pos{pose.x, pose.y, pose.z}, true, pose.yaw);

    // Fable's own AI keeps handing the creature somewhere to walk. Dropping its actions
    // regularly is cheaper than fighting it every frame, and the position write above is
    // what actually decides where it stands.
    if (nowMs - p.lastCommandsMs >= kClearCommandsEveryMs) {
        p.lastCommandsMs = nowMs;
        script::clear_commands(p.ref);
    }
}

void PuppetPool::sync(const std::vector<world::Spawn>& spawned, const std::vector<uint32_t>& removed,
                      const std::vector<world::Pose>& poses, uint32_t nowMs) {
    for (uint32_t id : removed) park(id);
    for (const world::Spawn& s : spawned) {
        if (live_.count(s.id)) continue;
        spawn_for(s, nowMs);
    }
    for (const world::Pose& p : poses) drive(p, nowMs);
    stats_.live = static_cast<uint32_t>(live_.size());
}

void PuppetPool::clear_all() {
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
