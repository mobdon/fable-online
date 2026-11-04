#pragma once
// How the local game shows the server's death. Pure logic, unit-tested in
// client-mod/tests/world_test.cpp; client-mod/src/sync.cpp makes the Fable calls.
//
// Fable's own death would end in its game-over screen and a reload, which an online game
// must never do. So when the server has the hero dead, the mod plays the fall itself, holds
// the hero where they fell, fades the screen out, and waits. The server then either brings
// the hero back where they fell (a Resurrection Phial) or moves them to the nearest Cullis
// Gate with an enter_region marked `respawn`; the mod makes the local game travel there,
// waits for the load, and fades back in.
#include <cstdint>
#include <string>

namespace fo::world {

constexpr uint32_t kDeathFadeDelayMs = 1200;  // the fall plays before the screen goes dark
constexpr uint32_t kDeathFadeOutMs = 1500;
constexpr uint32_t kDeathFadeInMs = 900;
/** No word from the server for this long while down: get up anyway, rather than stay dark. */
constexpr uint32_t kDeathGiveUpMs = 60000;

class DeathView {
  public:
    enum class Phase : uint8_t { Alive, Down, Moving, Rising };

    /** What sync.cpp should do this frame. Each flag is true once, on the frame it happens. */
    struct Step {
        bool fell = false;     // play the fall, stop local damage, start holding the hero
        bool travel = false;   // start the local move to `gate`
        bool rose = false;     // let go: animations, damage and position are the player's again
    };

    /**
     * Once a frame. `serverHealth`/`serverMax` are the own hero's last tick (max 0 when the
     * frame had none); `loading` is true while a region change waits for the local load.
     */
    Step update(float serverHealth, float serverMax, bool loading, uint32_t nowMs);
    /** The server is moving the hero to a gate because they died (enter_region "respawn"). */
    void respawn_at(const std::string& gate, uint32_t nowMs);
    /** Offline or a new connection: drop everything and show the game again. */
    void reset() { *this = DeathView(); }

    Phase phase() const { return phase_; }
    bool down() const { return phase_ == Phase::Down || phase_ == Phase::Moving; }
    const std::string& gate() const { return gate_; }
    /** How dark the screen is, 0 (clear) to 1 (black). */
    float fade(uint32_t nowMs) const;

  private:
    Phase phase_ = Phase::Alive;
    uint32_t sinceMs_ = 0;  // when the current phase began
    uint32_t downMs_ = 0;
    bool travelPending_ = false;
    bool sawLoading_ = false;
    std::string gate_;
};

}  // namespace fo::world
