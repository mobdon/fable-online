#pragma once
// When the mod tells the server where the local hero is, and how it keeps the link alive.
// No Windows or game dependencies, so it is tested on its own (tests/world_test.cpp).
//
// The server checks every report against how far the hero could have moved
// (server/src/world/movement.ts), so reports go out often while the hero moves and stop when
// it stands still: silence is how the server knows the stand-in should idle. A slow
// heartbeat report still goes out, so a hero standing still is not mistaken for a lost one.
#include <cstdint>

namespace fo::world {

struct ReportPolicy {
    uint32_t minIntervalMs = 50;    // at most 20 reports a second (the server allows 60)
    uint32_t heartbeatMs = 1000;    // one report a second while standing still
    float minMove = 0.05f;          // metres that count as moving
    float minTurn = 0.05f;          // radians that count as turning
    uint32_t pingMs = 2000;         // round trip measurement
};

class PositionReporter {
public:
    explicit PositionReporter(ReportPolicy p = {}) : p_(p) {}

    /** True when a report with this position and facing should be sent now. Call `sent` after. */
    bool due(uint32_t nowMs, float x, float y, float z, float yaw) const;
    void sent(uint32_t nowMs, float x, float y, float z, float yaw);
    /** After a server correction or a region change: report the next position straight away. */
    void reset();

    /** True when a ping should go out now (and records that one did). */
    bool ping_due(uint32_t nowMs);
    /** A pong came back carrying the ping's send time. */
    void pong(uint32_t nowMs, double sentMs);
    /** Smoothed round trip in ms, or -1 before the first pong. */
    int rtt_ms() const { return rtt_ < 0 ? -1 : static_cast<int>(rtt_ + 0.5f); }

private:
    ReportPolicy p_;
    bool have_ = false;
    uint32_t lastMs_ = 0;
    float x_ = 0, y_ = 0, z_ = 0, yaw_ = 0;
    bool pinged_ = false;
    uint32_t lastPingMs_ = 0;
    float rtt_ = -1;
};

}  // namespace fo::world
