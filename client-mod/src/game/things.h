#pragma once
// Applies the server's answers about doors, chests and items on the ground to Fable's own
// things (docs/world-objects.md, "The client side").
//
// Not mapped yet: finding a CThing by its level and UID, and the CTCDoor / CTCChest /
// CTCInventoryItem calls that open a door or hide an item. Until then each change is
// logged and counted, so a test session shows what the server sent. The vtables are in
// docs/world-objects.md.
#include <cstdint>

#include "../world/objects.h"

namespace fo::game::things {

struct Stats {
    uint32_t applied = 0;
    uint32_t unmapped = 0;
};

/** Makes the local thing match the server: swing a door, hide or show an item, set a chest open. */
void apply(const world::ObjectChange& change);
const Stats& stats();

/**
 * True once the local game has finished a region load after a region change: the hero
 * exists again and is in a different area than before the change. `areaBefore` is
 * game::area_id of the hero when the change began (0 if there was no hero).
 */
bool region_loaded(uint32_t areaBefore);

}  // namespace fo::game::things
