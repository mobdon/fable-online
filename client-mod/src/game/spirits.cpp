#include "spirits.h"

#include <cstdio>

#include "../log.h"
#include "thing.h"

namespace fo::game {
namespace {
/** The spirits' defs, in the order of the party's colour slots. */
constexpr const char* kDefs[4] = {"COOP_SPIRIT_PLAYER_ONE", "COOP_SPIRIT_PLAYER_TWO", "COOP_SPIRIT_PLAYER_THREE",
                                  "COOP_SPIRIT_PLAYER_FOUR"};
/** Metres above the stand-in's feet: over the head, under the name tag. */
constexpr float kAboveHead = 2.6f;
constexpr uint32_t kCreateEveryMs = 500;
constexpr uint32_t kClearCommandsEveryMs = 1000;
}  // namespace

void PartySpirits::set_members(std::vector<std::pair<uint32_t, int>> members) {
    members_.clear();
    for (auto& [id, slot] : members)
        if (slot >= 0 && slot < 4) members_[id] = slot;
    std::vector<uint32_t> gone;
    for (auto& [id, s] : live_) {
        auto it = members_.find(id);
        if (it == members_.end() || it->second != s.slot) gone.push_back(id);
    }
    for (uint32_t id : gone) drop(id);
}

void PartySpirits::drop(uint32_t id) {
    auto it = live_.find(id);
    if (it == live_.end()) return;
    if (it->second.ref.alive()) {
        script::set_drawable(it->second.ref, false);
        script::set_free(it->second.ref);
    }
    it->second.ref.release();
    live_.erase(it);
}

void PartySpirits::fail(const char* why) {
    fo::log("spirits: %s; party spirits are off for this session (the orbs still mark the party)", why);
    failed_ = true;
    clear_all();
}

void PartySpirits::clear_all() {
    std::vector<uint32_t> ids;
    for (auto& [id, s] : live_) ids.push_back(id);
    for (uint32_t id : ids) drop(id);
}

void PartySpirits::sync(const std::vector<world::Pose>& poses, bool may_create, uint32_t nowMs) {
    if (!enabled() || script::disabled()) return;
    std::unordered_map<uint32_t, const world::Pose*> seen;
    for (const world::Pose& p : poses)
        if (p.kind == world::Kind::Hero && members_.count(p.id)) seen[p.id] = &p;

    for (auto& [id, pose] : seen) {
        auto it = live_.find(id);
        if (it == live_.end()) {
            if (!may_create || nowMs - lastCreateMs_ < kCreateEveryMs) continue;
            lastCreateMs_ = nowMs;
            int slot = members_[id];
            char name[48];
            std::snprintf(name, sizeof(name), "FO_SPIRIT_%u_%u", id, nextName_++);
            Spirit s;
            s.slot = slot;
            if (!script::create_near(kDefs[slot], name, 4.0f, s.ref)) return fail("could not create a spirit");
            script::set_attackable(s.ref, false);
            script::set_damageable(s.ref, false);
            script::clear_commands(s.ref);
            s.lastCommandsMs = nowMs;
            if (!physics_of(s.ref.thing())) {
                script::set_free(s.ref);
                return fail("the spirit has no physics to move it by");
            }
            fo::log("spirits: %s over party member %u", kDefs[slot], id);
            it = live_.emplace(id, std::move(s)).first;
        }
        Spirit& s = it->second;
        uintptr_t thing = s.ref.thing();
        if (!thing || !s.ref.alive()) {  // freed by an area load: made again later
            s.ref.release();
            live_.erase(it);
            continue;
        }
        if (s.hidden) {
            script::set_drawable(s.ref, true);
            s.hidden = false;
        }
        write_pos(thing, Pos{pose->x, pose->y, pose->z + kAboveHead}, true, pose->yaw);
        if (nowMs - s.lastCommandsMs >= kClearCommandsEveryMs) {
            s.lastCommandsMs = nowMs;
            script::clear_commands(s.ref);
        }
    }
    // A member whose stand-in is out of sight: hide the spirit rather than leave it hanging.
    for (auto& [id, s] : live_)
        if (!seen.count(id) && !s.hidden && s.ref.alive()) {
            script::set_drawable(s.ref, false);
            s.hidden = true;
        }
}

}  // namespace fo::game
