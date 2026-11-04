#include "reporter.h"

#include <cmath>

namespace fo::world {
namespace {

float angle_between(float a, float b) {
    float d = std::fmod(a - b, 6.2831853f);
    if (d > 3.1415927f) d -= 6.2831853f;
    if (d < -3.1415927f) d += 6.2831853f;
    return std::fabs(d);
}

}  // namespace

bool PositionReporter::due(uint32_t nowMs, float x, float y, float z, float yaw) const {
    if (!have_) return true;
    uint32_t since = nowMs - lastMs_;
    if (since < p_.minIntervalMs) return false;
    if (since >= p_.heartbeatMs) return true;
    float moved = std::sqrt((x - x_) * (x - x_) + (y - y_) * (y - y_) + (z - z_) * (z - z_));
    return moved >= p_.minMove || angle_between(yaw, yaw_) >= p_.minTurn;
}

void PositionReporter::sent(uint32_t nowMs, float x, float y, float z, float yaw) {
    have_ = true;
    lastMs_ = nowMs;
    x_ = x;
    y_ = y;
    z_ = z;
    yaw_ = yaw;
}

void PositionReporter::reset() { have_ = false; }

bool PositionReporter::ping_due(uint32_t nowMs) {
    if (pinged_ && nowMs - lastPingMs_ < p_.pingMs) return false;
    pinged_ = true;
    lastPingMs_ = nowMs;
    return true;
}

void PositionReporter::pong(uint32_t nowMs, double sentMs) {
    if (!std::isfinite(sentMs) || sentMs < 0 || sentMs > 4294967295.0) return;
    uint32_t rtt = nowMs - static_cast<uint32_t>(sentMs);
    if (rtt > 60000) return;  // not one of ours
    rtt_ = rtt_ < 0 ? static_cast<float>(rtt) : rtt_ * 0.8f + static_cast<float>(rtt) * 0.2f;
}

}  // namespace fo::world
