#pragma once
// The weapons the local hero has equipped in Fable, by def name (tools/re-notes/13-hero-gear.md).
// Read-only: nothing here changes the game.
#include <cstdint>
#include <string>

namespace fo::game {

struct HeroGear {
    std::string melee;   // e.g. OBJECT_SWORD_OF_AEONS; "" for none
    std::string ranged;  // e.g. OBJECT_YEW_LONGBOW; "" for none
};

/**
 * Reads the hero's CTCInventoryWeapons component. False when the hero or the component can't
 * be read (between areas); `out` is then left as it was.
 */
bool read_gear(uintptr_t hero, HeroGear& out);

}  // namespace fo::game
