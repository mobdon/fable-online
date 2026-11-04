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
};

bool read(State& out);
/** Rain at `amount` (0..1); a negative amount gives the area its own weather back. */
bool set_rain(float amount);
bool set_snow(float amount);

}  // namespace fo::game::weather
