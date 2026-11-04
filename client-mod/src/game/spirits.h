#pragma once
// Party spirits: the four coloured spirit creatures of Fable's unused same-screen co-op mode
// (COOP_SPIRIT_PLAYER_ONE..FOUR, red, blue, yellow and green; tools/re-notes/09-coop-spirit.md),
// made in the local game and held over each party member's stand-in as a marker.
//
// Only the creature is used, never the co-op mode: it is moved by position writes like a
// puppet, and the local game may not hurt or target it. It has not been run in game yet, so
// it is off unless FABLE_ONLINE_PARTY_SPIRITS=1; the overlay's coloured orbs mark party
// members either way. One failure (no creature, no physics to move it by) turns it off for
// the session.
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../world/entities.h"
#include "script.h"

namespace fo::game {

class PartySpirits {
  public:
    void set_enabled(bool on) { enabled_ = on; }
    bool enabled() const { return enabled_ && !failed_; }

    /** Party members other than this player: (hero id, colour slot 0..3). */
    void set_members(std::vector<std::pair<uint32_t, int>> members);

    /**
     * Keeps a spirit over each member whose stand-in is in `poses`. `may_create` is false
     * while the local game is loading (making creatures then crashes it).
     */
    void sync(const std::vector<world::Pose>& poses, bool may_create, uint32_t nowMs);

    /** Lets every spirit go: on a region change, a disconnect, or leaving the party. */
    void clear_all();

  private:
    struct Spirit {
        script::ThingRef ref;
        int slot = 0;
        bool hidden = false;
        uint32_t lastCommandsMs = 0;
    };
    void drop(uint32_t id);
    void fail(const char* why);

    bool enabled_ = false;
    bool failed_ = false;
    std::unordered_map<uint32_t, int> members_;
    std::unordered_map<uint32_t, Spirit> live_;
    uint32_t lastCreateMs_ = 0;
    uint32_t nextName_ = 1;
};

}  // namespace fo::game
