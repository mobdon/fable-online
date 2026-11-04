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

namespace fo::game {

struct PuppetStats {
    uint32_t spawned = 0;
    uint32_t spawnFailed = 0;
    uint32_t reused = 0;
    uint32_t parked = 0;
    uint32_t lost = 0;  // the engine freed the thing under us
    uint32_t live = 0;
};

class PuppetPool {
  public:
    /** `heroDef` is the creature def used to stand in for another player. */
    explicit PuppetPool(std::string heroDef) : heroDef_(std::move(heroDef)) {}

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
    };

    bool spawn_for(const world::Spawn& s, uint32_t nowMs);
    void park(uint32_t id);
    void drive(const world::Pose& p, uint32_t nowMs);
    const std::string& def_for(const world::Spawn& s) const;

    std::string heroDef_;
    std::unordered_map<uint32_t, Puppet> live_;
    /** Parked puppets by def, ready to be shown again instead of spawning another. */
    std::unordered_multimap<std::string, Puppet> pool_;
    PuppetStats stats_;
    uint32_t nextName_ = 1;
    /** Entities waiting for a puppet (held, or lost to an area load and to be made again). */
    std::unordered_map<uint32_t, world::Spawn> pending_;
    uint32_t holdUntil_ = 0;
    uint32_t lastSpawnMs_ = 0;
};

}  // namespace fo::game
