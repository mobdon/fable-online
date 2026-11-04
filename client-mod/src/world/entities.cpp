#include "entities.h"

#include <algorithm>
#include <cmath>

namespace fo::world {
namespace {
constexpr float kPi = 3.14159265358979323846f;

float wrap_angle(float a) {
    const float two = kPi * 2;
    a = std::fmod(a + kPi, two);
    if (a < 0) a += two;
    return a - kPi;
}
}  // namespace

float angle_delta(float from, float to) { return wrap_angle(to - from); }

float lerp_angle(float from, float to, float t) { return wrap_angle(from + angle_delta(from, to) * t); }

void EntityTable::clear() {
    for (const auto& [id, e] : live_) removed_.push_back(id);
    live_.clear();
    spawned_.clear();
    lastTick_ = 0;
    haveOwn_ = false;
}

void EntityTable::on_tick(const Tick& t, uint32_t nowMs) {
    // Frames can arrive out of order. An older frame carries nothing newer than what we
    // already have, so applying it would rewind every entity.
    if (t.tick != 0 && lastTick_ != 0 && t.tick <= lastTick_) {
        ++staleFrames_;
        return;
    }
    lastTick_ = t.tick;

    for (const EntityState& s : t.entities) {
        if (s.id == localId_) {
            // Our own hero is driven by the player, not by us; only its server state is kept.
            own_ = s;
            haveOwn_ = true;
            continue;
        }
        auto it = live_.find(s.id);
        if (it == live_.end()) {
            Entry e;
            e.meta.id = s.id;
            e.meta.kind = s.kind;
            e.meta.def = s.def;
            e.meta.name = s.name;
            it = live_.emplace(s.id, std::move(e)).first;
            spawned_.push_back(Spawn{s.id, s.kind, s.def, s.name, s.x, s.y, s.z, s.yaw});
        }
        Entry& e = it->second;
        // A name or renown can change while a hero is in the region; a def never does.
        if (!s.name.empty()) e.meta.name = s.name;
        if (s.renown) e.meta.renown = s.renown;
        e.meta.health = s.health;
        e.meta.anim = s.anim;
        e.meta.dead = s.anim == Anim::Death || s.health <= 0;
        e.meta.lastSeenMs = nowMs;
        if (!e.history.empty() && nowMs < e.history.back().atMs) e.history.clear();  // clock went backwards
        e.history.push_back(Sample{nowMs, s.x, s.y, s.z, s.yaw});
        while (e.history.size() > 2 && nowMs - e.history.front().atMs > kHistoryMs) e.history.pop_front();
    }

    for (auto it = live_.begin(); it != live_.end();) {
        if (nowMs - it->second.meta.lastSeenMs > kRemoveAfterMs) {
            removed_.push_back(it->first);
            it = live_.erase(it);
        } else {
            ++it;
        }
    }
}

std::vector<Spawn> EntityTable::drain_spawned() {
    std::vector<Spawn> out;
    out.swap(spawned_);
    return out;
}

std::vector<uint32_t> EntityTable::drain_removed() {
    std::vector<uint32_t> out;
    out.swap(removed_);
    return out;
}

const Remote* EntityTable::find(uint32_t id) const {
    auto it = live_.find(id);
    return it == live_.end() ? nullptr : &it->second.meta;
}

bool EntityTable::pose_of(uint32_t id, uint32_t nowMs, Pose& out) const {
    auto it = live_.find(id);
    if (it == live_.end()) return false;
    const Entry& e = it->second;
    if (e.history.empty()) return false;

    // Render in the past so there is a frame on each side of the time we draw.
    uint32_t at = nowMs > kInterpDelayMs ? nowMs - kInterpDelayMs : 0;
    const Sample* before = nullptr;
    const Sample* after = nullptr;
    for (const Sample& s : e.history) {
        if (s.atMs <= at) before = &s;
        else if (!after) after = &s;
    }

    out = Pose{};
    out.id = e.meta.id;
    out.kind = e.meta.kind;
    out.anim = e.meta.anim;
    out.health = e.meta.health;

    if (before && after && after->atMs > before->atMs) {
        float span = static_cast<float>(after->atMs - before->atMs);
        float f = static_cast<float>(at - before->atMs) / span;
        out.x = before->x + (after->x - before->x) * f;
        out.y = before->y + (after->y - before->y) * f;
        out.z = before->z + (after->z - before->z) * f;
        out.yaw = lerp_angle(before->yaw, after->yaw, f);
        float dx = after->x - before->x, dy = after->y - before->y;
        out.speed = std::sqrt(dx * dx + dy * dy) / (span / 1000.0f);
    } else {
        // Before the first frame, or past the newest one: hold the nearest pose rather
        // than guessing ahead, which would make an entity slide through walls.
        const Sample& s = before ? *before : e.history.front();
        out.x = s.x;
        out.y = s.y;
        out.z = s.z;
        out.yaw = s.yaw;
        if (e.history.size() >= 2) {
            const Sample& a = e.history[e.history.size() - 2];
            const Sample& b = e.history.back();
            if (b.atMs > a.atMs) {
                float dx = b.x - a.x, dy = b.y - a.y;
                out.speed = std::sqrt(dx * dx + dy * dy) / ((b.atMs - a.atMs) / 1000.0f);
            }
        }
    }
    return true;
}

std::vector<Pose> EntityTable::poses(uint32_t nowMs) const {
    std::vector<Pose> out;
    out.reserve(live_.size());
    for (const auto& [id, e] : live_) {
        Pose p;
        if (pose_of(id, nowMs, p)) out.push_back(p);
    }
    // A stable order keeps the log readable and the puppet work deterministic.
    std::sort(out.begin(), out.end(), [](const Pose& a, const Pose& b) { return a.id < b.id; });
    return out;
}

}  // namespace fo::world
