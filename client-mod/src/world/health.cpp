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

}  // namespace fo::world
