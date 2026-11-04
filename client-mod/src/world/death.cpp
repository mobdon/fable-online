#include "death.h"

namespace fo::world {
namespace {
float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
}  // namespace

DeathView::Step DeathView::update(float serverHealth, float serverMax, bool loading, uint32_t nowMs) {
    Step s;
    bool serverDead = serverMax > 0 && serverHealth <= 0;
    switch (phase_) {
    case Phase::Alive:
    case Phase::Rising:
        if (phase_ == Phase::Rising && nowMs - sinceMs_ >= kDeathFadeInMs) phase_ = Phase::Alive;
        if (serverDead) {
            phase_ = Phase::Down;
            sinceMs_ = downMs_ = nowMs;
            gate_.clear();
            travelPending_ = sawLoading_ = false;
            s.fell = true;
        }
        return s;
    case Phase::Down:
        if (travelPending_) {
            phase_ = Phase::Moving;
            sinceMs_ = nowMs;
            travelPending_ = false;
            s.travel = true;
            return s;
        }
        // Back up where they fell (a phial), or the server gave up on us: rise here.
        if ((serverMax > 0 && serverHealth > 0) || nowMs - downMs_ >= kDeathGiveUpMs) {
            phase_ = Phase::Rising;
            sinceMs_ = nowMs;
            s.rose = true;
        }
        return s;
    case Phase::Moving:
        if (loading) sawLoading_ = true;
        // The local load has finished (or never started and the server let us go).
        if ((sawLoading_ && !loading) || (!loading && nowMs - sinceMs_ >= kDeathGiveUpMs / 2) ||
            nowMs - downMs_ >= kDeathGiveUpMs * 2) {
            phase_ = Phase::Rising;
            sinceMs_ = nowMs;
            s.rose = true;
        }
        return s;
    }
    return s;
}

void DeathView::respawn_at(const std::string& gate, uint32_t nowMs) {
    gate_ = gate;
    if (phase_ == Phase::Alive || phase_ == Phase::Rising) {
        // The tick with health 0 never reached us (a dropped frame): treat it as a death now.
        phase_ = Phase::Down;
        sinceMs_ = downMs_ = nowMs - kDeathFadeDelayMs - kDeathFadeOutMs;
    }
    if (phase_ == Phase::Down) travelPending_ = true;
}

float DeathView::fade(uint32_t nowMs) const {
    switch (phase_) {
    case Phase::Alive: return 0;
    case Phase::Down: {
        uint32_t t = nowMs - downMs_;
        if (t < kDeathFadeDelayMs) return 0;
        return clamp01(static_cast<float>(t - kDeathFadeDelayMs) / kDeathFadeOutMs);
    }
    case Phase::Moving: return 1;
    case Phase::Rising: return clamp01(1.0f - static_cast<float>(nowMs - sinceMs_) / kDeathFadeInMs);
    }
    return 0;
}

}  // namespace fo::world
