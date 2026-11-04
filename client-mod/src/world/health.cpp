#include "health.h"

#include <cmath>

namespace fo::world {

float health_target(float serverHealth, float serverMax, float fableHealth, float fableMax) {
    auto ok = [](float v) { return std::isfinite(v) && v >= 0 && v < 1.0e6f; };
    if (!ok(serverHealth) || !ok(serverMax) || !ok(fableHealth) || !ok(fableMax)) return -1;
    if (serverMax <= 0 || fableMax <= 0) return -1;
    float share = serverHealth >= serverMax ? 1.0f : serverHealth / serverMax;
    float want = share * fableMax;
    if (want < 1.0f) want = 1.0f;  // downed or nearly: hold at 1 (see the header)
    if (want > fableMax) want = fableMax;
    if (std::fabs(want - fableHealth) < kHealthSlack) return -1;
    return want;
}

void HealWatch::observe(float health, float maxHealth, uint32_t nowMs) {
    if (!std::isfinite(health) || !std::isfinite(maxHealth) || maxHealth <= 0) return;
    if (last_ >= 0 && health > last_ + kHealthSlack) {
        if (!gathering_) {
            gathering_ = true;
            firstMs_ = nowMs;
            gathered_ = 0;
        }
        gathered_ += (health - last_) / maxHealth;
        holdUntilMs_ = nowMs + kHealGatherMs + kHealWaitMs;
    }
    last_ = health;
}

bool HealWatch::take_report(uint32_t nowMs, float& share) {
    if (!gathering_ || nowMs - firstMs_ < kHealGatherMs) return false;
    gathering_ = false;
    share = gathered_ > 2.0f ? 2.0f : gathered_;
    holdUntilMs_ = nowMs + kHealWaitMs;
    return share > 0;
}

bool HealWatch::holding(uint32_t nowMs) const {
    return gathering_ || static_cast<int32_t>(holdUntilMs_ - nowMs) > 0;
}

}  // namespace fo::world
