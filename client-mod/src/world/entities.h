#pragma once
// The entity table: what the server says is in this region, and where each entity should
// be drawn right now. Shared by the stand-in heroes (other players) and the server's
// creatures, because both are the same problem: a remote entity the local game must show.
//
// Pure logic, no Windows or game calls, so it is unit-tested (client-mod/tests/world_test.cpp).
// client-mod/src/game/puppet.h turns the poses this produces into Fable calls.
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "snapshot.h"

namespace fo::world {

/** Rendering runs this far behind the newest frame so jitter between frames is absorbed. */
constexpr uint32_t kInterpDelayMs = 150;  // players report 10 times a second
/** An entity missing from frames for this long is gone (a dropped frame must not despawn it). */
constexpr uint32_t kRemoveAfterMs = 2000;
/** Poses older than this are of no use for interpolation. */
constexpr uint32_t kHistoryMs = 1000;
/** Frames whose arrival times are compared to find the server clock offset. */
constexpr uint32_t kOffsetWindowMs = 2000;

struct Pose {
    uint32_t id = 0;
    Kind kind = Kind::Creature;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    Anim anim = Anim::Idle;
    float health = 0;
    /** Speed along the ground in metres per second, from the frames themselves. */
    float speed = 0;
};

struct Spawn {
    uint32_t id = 0;
    Kind kind = Kind::Creature;
    std::string def;
    std::string name;
    float x = 0, y = 0, z = 0;
    float yaw = 0;
};

struct Remote {
    uint32_t id = 0;
    Kind kind = Kind::Creature;
    std::string def;
    std::string name;
    uint32_t renown = 0;
    float health = 0;
    Anim anim = Anim::Idle;
    uint32_t lastSeenMs = 0;
    bool dead = false;
};

/**
 * Takes the server's frames in, gives interpolated poses out.
 *
 * `nowMs` is any monotonic millisecond clock (GetTickCount in the mod, a counter in tests).
 * Both calls must use the same clock.
 */
class EntityTable {
  public:
    /** The id of this player's own hero, which is never given a puppet. */
    void set_local_id(uint32_t id) { localId_ = id; }
    uint32_t local_id() const { return localId_; }

    /** Clears everything: used on a region change, where no id carries over. */
    void clear();

    void on_tick(const Tick& t, uint32_t nowMs);
    /**
     * The server's tick rate (from `welcome`). With it, each frame is stamped from its tick
     * number instead of its arrival time, so network and polling jitter does not show up
     * as stutter in the interpolated poses.
     */
    void set_tick_rate(uint32_t hz) { tickRate_ = hz; offsets_.clear(); }

    /** Entities that appeared since the last drain, oldest first. */
    std::vector<Spawn> drain_spawned();
    /** Entities that have gone (left the region, despawned, or stopped being sent). */
    std::vector<uint32_t> drain_removed();

    /** Where every live remote entity should be drawn at `nowMs`. */
    std::vector<Pose> poses(uint32_t nowMs) const;
    /** The pose of one entity, if it is still live. */
    bool pose_of(uint32_t id, uint32_t nowMs, Pose& out) const;

    size_t size() const { return live_.size(); }
    const Remote* find(uint32_t id) const;
    /**
     * This player's own hero as the server last had it (its health, for the local hero).
     * False until a frame has carried it.
     */
    bool own_state(EntityState& out) const {
        if (!haveOwn_) return false;
        out = own_;
        return true;
    }
    /** Frames whose tick number went backwards (a stale or duplicated frame). */
    uint32_t stale_frames() const { return staleFrames_; }

  private:
    struct Sample {
        uint32_t atMs;
        float x, y, z, yaw;
    };
    struct Entry {
        Remote meta;
        std::deque<Sample> history;
    };

    std::unordered_map<uint32_t, Entry> live_;
    std::vector<Spawn> spawned_;
    std::vector<uint32_t> removed_;
    uint32_t localId_ = 0;
    EntityState own_;
    bool haveOwn_ = false;
    uint32_t stamp(uint32_t tick, uint32_t nowMs);
    uint32_t lastTick_ = 0;
    uint32_t tickRate_ = 0;
    /** (arrival, local clock minus server time) for the last kOffsetWindowMs of frames. */
    std::deque<std::pair<uint32_t, double>> offsets_;
    uint32_t staleFrames_ = 0;
};

/** Shortest signed turn from `from` to `to`, both in radians. */
float angle_delta(float from, float to);
/** Interpolates an angle the short way round. */
float lerp_angle(float from, float to, float t);

}  // namespace fo::world
