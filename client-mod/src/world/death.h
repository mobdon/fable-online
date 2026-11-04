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
/**
 * Health back above 0 while down: wait this long before getting up in place. The server
 * revives first and sends the respawn move just after, so rising at once would get up,
 * then fall again a moment later. A phial revive (2 s) simply waits this out.
 */
constexpr uint32_t kRiseGraceMs = 1500;
/** Within this many metres of the respawn point, the local move has landed. */
constexpr float kArrivedMetres = 60.0f;

class DeathView {
  public:
    enum class Phase : uint8_t { Alive, Down, Moving, Rising };

    /** What sync.cpp should do this frame. Each flag is true once, on the frame it happens. */
    struct Step {
        bool fell = false;     // play the fall, stop local damage, start holding the hero
        bool travel = false;   // start the local move to `gate`
        bool rose = false;     // let go: damage and position are the player's again
        bool roseInPlace = false;  // with `rose`: no gate move happened, so the fall is still queued
    };

    /**
     * Once a frame. `serverHealth`/`serverMax` are the own hero's last tick (max 0 when the
     * frame had none); `loading` is true while a region change waits for the local load.
     */
    Step update(float serverHealth, float serverMax, bool loading, uint32_t nowMs);
    /** The server is moving the hero to a gate because they died (enter_region "respawn"). */
    void respawn_at(const std::string& gate, float x, float y, uint32_t nowMs);
    /** Where the server put the hero for the respawn (world metres). */
    float target_x() const { return tx_; }
    float target_y() const { return ty_; }
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
    uint32_t aliveSinceMs_ = 0;  // while down: when the server's health came back, 0 if not
    float tx_ = 0, ty_ = 0;
    bool travelPending_ = false;
    bool sawLoading_ = false;
    std::string gate_;
};

}  // namespace fo::world
