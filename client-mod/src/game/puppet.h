#pragma once
// Puppets: the local creatures that stand in for things the server owns. One puppet per
// remote entity, whether that entity is another player's hero or one of the server's
// monsters, because the local game draws both the same way.
//
// The server decides where they are (client-mod/src/world/entities.h); this file only
// makes the local game show it.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../world/entities.h"
#include "script.h"
#include "thing.h"

namespace fo::game {

struct PuppetStats {
    uint32_t spawned = 0;
    uint32_t spawnFailed = 0;
    uint32_t reused = 0;
    uint32_t parked = 0;
    uint32_t lost = 0;  // the engine freed the thing under us
    uint32_t removed = 0;  // retired puppets taken out of the world
    uint32_t live = 0;
    uint32_t animsPlayed = 0;
    uint32_t animsRefused = 0;  // the engine fields the animation code reads were not valid
    uint32_t healthLogs = 0;
    uint32_t described = 0;
};

/** What a puppet is playing, picked from the server's anim field (world::Anim). */
enum class Gait : uint8_t { None, Idle, Walk, Run, Attack, Alert, Hit, Death, Leap, Block, Taunt };

class PuppetPool {
  public:
    /** `heroDef` is the creature def used to stand in for another player. */
    explicit PuppetPool(std::string heroDef) : heroDef_(std::move(heroDef)) {}

    /** Off: puppets are only moved, never animated (the behaviour before gaits). */
    void set_animate(bool on) { animate_ = on; }
    /** New creature puppets go into Fable's scripted mode so their own AI stays off. */
    void set_scripted_mode(bool on) { scriptedMode_ = on; }

    /** Applies one round of table output. Must run on a thread that may call the engine. */
    void sync(const std::vector<world::Spawn>& spawned, const std::vector<uint32_t>& removed,
              const std::vector<world::Pose>& poses, uint32_t nowMs);

    /** Parks every puppet and drops the pool: on a region change or on disconnect. */
    void clear_all();

    /**
     * No new puppets until `ms` from now. Creating creatures while Fable is still loading an
     * area, or just after it freed the old ones, crashes its renderer. Spawns asked for in the
     * meantime wait and go out one at a time afterwards.
     */
    void hold(uint32_t nowMs, uint32_t ms);
    bool holding(uint32_t nowMs) const { return static_cast<int32_t>(holdUntil_ - nowMs) > 0; }

    /**
     * Puts every live puppet where its pose says, with memory writes only (no engine
     * calls), so it can run once a frame between the slower sync() steps.
     */
    void place(const std::vector<world::Pose>& poses);

    const PuppetStats& stats() const { return stats_; }
    size_t live_count() const { return live_.size(); }

  private:
    struct Puppet {
        world::Spawn spawn;
        script::ThingRef ref;
        std::string def;
        std::string scriptName;
        uint32_t lastCommandsMs = 0;
        bool hidden = false;
        uint32_t shownAfterMs = 0;  // when a hidden puppet that has been placed may be shown
        float lastHealth = -1;
        Gait gait = Gait::None;
        Gait wanted = Gait::None;  // the gait the server asks for, waiting out kGaitSettleMs
        uint32_t wantedSinceMs = 0;
        uint32_t lastAnimMs = 0;
        uint32_t animSeq = 0;
        uint32_t strikes = 0;  // alternates left and right swings
        uint32_t createdMs = 0;  // shown (made or taken from the pool) at this time
        bool described = false;
        bool animOk = false;  // the last animation was taken
        uint32_t flinchUntilMs = 0;
        uint32_t retryAtMs = 0;  // a refused animation is tried again from this time
        uintptr_t thingAddr = 0;  // the CThing, kept out of the hero scan
        Pos parkedAt;             // where a parked puppet is held, far from everyone
        bool parkedAway = false;
        uint32_t deadSinceMs = 0;  // the server said it died at this time (0: alive)
        Pos diedAt;                // where it fell; it is kept there
    };

    bool spawn_for(const world::Spawn& s, uint32_t nowMs);
    void park(uint32_t id);
    void send_away(Puppet& p);
    void hold_parked(uint32_t nowMs);
    void retire(Puppet& p);
    bool may_remove() const;
    void drive(const world::Pose& p, uint32_t nowMs);
    void animate(Puppet& p, const world::Pose& pose, uint32_t nowMs);
    const std::string& def_for(const world::Spawn& s) const;

    std::string heroDef_;
    bool animate_ = true;
    bool scriptedMode_ = true;
    std::unordered_map<uint32_t, Puppet> live_;
    /** Parked puppets by def, ready to be shown again instead of spawning another. */
    std::unordered_multimap<std::string, Puppet> pool_;
    uint32_t lastParkedHoldMs_ = 0;
    PuppetStats stats_;
    uint32_t nextName_ = 1;
    /** Entities waiting for a puppet (held, or lost to an area load and to be made again). */
    std::unordered_map<uint32_t, world::Spawn> pending_;
    uint32_t holdUntil_ = 0;
    uint32_t lastSpawnMs_ = 0;
    uint32_t lastSyncMs_ = 0;
    uint32_t gameThread_ = 0;  // the thread sync() runs on: the only one that may remove things
};

}  // namespace fo::game
