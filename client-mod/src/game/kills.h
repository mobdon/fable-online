#pragma once
// Client monsters: monsters run in the player's own Fable. Every health change goes through
// one engine function (0x006A62E0, under the things' ModifyHealth); the mod's wrapper sees
// each monster's death (reported to the server, which pays it or not) and each hit on the
// hero (reported as a share of max health, the server keeps the hero's health). The hero is
// never taken below 1 here: the server decides when the hero is down.
#include <cstdint>
#include <string>
#include <vector>

namespace fo::game::kills {

struct Kill {
    std::string def;
    float x = 0, y = 0, z = 0;
};

/** Points the three calls of the health function at the mod. Once, at start-up. */
bool install();
/** Reports are gathered only while on (the server's welcome said client monsters). */
void set_enabled(bool on);
bool enabled();
/** Deaths and hero damage since the last call. */
std::vector<Kill> drain_kills();
/** Share of the hero's max health lost since the last call (0 for none). */
float drain_hurt();

}  // namespace fo::game::kills
