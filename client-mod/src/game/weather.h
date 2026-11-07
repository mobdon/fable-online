#pragma once
// Fable's rain and snow: CEngineWeatherRenderer at [0x01436E44] (tools/re-notes/08-world-time.md).
// Fable's own quests use the same override (CEngine slots 80 and 81). With the override on, the
// renderer fades its rain or snow toward the amount by 0.01 a frame, so changes are never a jump.
// Game thread only.
namespace fo::game::weather {

struct State {
    bool found = false;
    /** What the renderer is drawing now, 0..1. */
    float rain = 0, snow = 0;
    /** The override, when one is on (-1 = Fable's own weather for the area). */
    float rainOverride = -1, snowOverride = -1;
    /** Whether each override flag is on, and the most the renderer draws (it caps overrides at this). */
    bool rainOn = false, snowOn = false;
    float most = 1.0f;
};

bool read(State& out);
/** Rain at `amount` (0..1); a negative amount gives the area its own weather back. */
bool set_rain(float amount);
bool set_snow(float amount);

/**
 * Fixes a retail bug: Fable's snow fade (0x00B52900) starts from the rain amount (+0x5C8)
 * instead of the snow amount (+0x5CC), so whenever rain falls snow follows it at nearly the
 * same strength. Rewrites that one displacement byte. Safe to call again; false if the
 * code is not what the Steam Fable.exe has there.
 */
bool fix_snow_fade();

}  // namespace fo::game::weather
