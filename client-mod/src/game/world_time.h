#pragma once
// Fable's clock: the CGameTimeManager that CWorld owns (tools/re-notes/08-world-time.md).
// Reads and writes go through Read/WriteProcessMemory, so a stale pointer fails instead of
// crashing the game. Game thread only.
#include <cstdint>

namespace fo::game::world_time {

struct Clock {
    uintptr_t manager = 0;
    /** Time of day, 0..1 (0 = midnight). */
    float time = 0;
    /** Fraction of a day per game second (1 / day length in seconds). */
    float speed = 0;
    /** Fable is easing the clock to a time a script asked for (a quest skipping to night). */
    bool transition = false;
    /** Something (a cutscene, a script) has stopped Fable's clock. */
    bool paused = false;
    /** Days Fable has counted since the save began. */
    uint32_t day = 0;
};

/** Finds the clock through [[[0x13B8A1C]+0x24]+0x1C] and reads it. False on menus and loads. */
bool read(Clock& out);
/** Puts the time of day at `fraction` (0..1). */
bool write_time(const Clock& c, float fraction);
/** Sets how fast Fable's clock runs, in days per game second. */
bool write_speed(const Clock& c, float speed);

}  // namespace fo::game::world_time
