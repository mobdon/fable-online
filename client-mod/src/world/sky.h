#pragma once
// The realm's sky: the server's `sky` frame (server/src/world/clock.ts) and the arithmetic that
// keeps Fable's own clock on it. Portable C++17, tested on the host (tests/sky_test.cpp); the
// game-side reads and writes are in game/world_time.cpp.
#include <cstdint>
#include <string>

namespace fo::world {

struct Sky {
    bool have = false;
    /** Time of day when the frame arrived, 0..1 (0 = midnight, 0.5 = noon). */
    double dayFraction = 0;
    /** Real milliseconds per game day. */
    double dayMs = 0;
    /** Local tick count when the frame arrived; the time runs on from there. */
    uint32_t receivedMs = 0;
    bool night = false;
    std::string climate, weather, previous;
    float intensity = 0;
    /** Server wall-clock ms the current weather began and ends. */
    double since = 0, until = 0;
    double fadeMs = 0;
};

/** Reads a `sky` frame. False for any other frame or a malformed one. */
bool parse_sky(const std::string& json, uint32_t nowMs, Sky& out);

/** The realm's time of day at `nowMs`, 0..1, run on from the last frame at the realm's speed. */
double sky_fraction(const Sky& s, uint32_t nowMs);

/** Shortest signed distance from `from` to `to` around the day, in (-0.5, 0.5]. */
double day_delta(double from, double to);

/** Hour and minute for the log. */
void day_hm(double fraction, int& hour, int& minute);

/**
 * What to do with Fable's clock this tick. Fable advances its time of day by `speed` (fraction
 * of a day per game second) every world update, so a small gap is closed by running a little
 * faster or slower for a few seconds (no visible jump in the light), and only a big gap (a join,
 * an area load, a staff time change, a paused game) is snapped.
 */
struct ClockStep {
    bool snap = false;
    double fraction = 0;  // where to put the clock when snapping
    double speed = 0;     // days per game second
};

/** Gaps bigger than this (in days; 0.02 = about 29 game minutes) are snapped. */
constexpr double kSnapGap = 0.02;
/** A smaller gap is closed over about this many seconds. */
constexpr double kCatchUpSeconds = 8.0;

ClockStep clock_step(double gameFraction, double realmFraction, double dayMs);

/**
 * Rain and snow for Fable's weather renderer, 0..1, or -1 to leave the area its own weather.
 * Only rain, storm, snow and blizzard are drawn; clear, cloudy and fog (which Fable has no
 * override for) and interiors give the area its own weather back.
 */
struct Precip {
    float rain = -1, snow = -1;
};
Precip precip_for(const Sky& s);

}  // namespace fo::world
