#pragma once
// The weapons the local hero has equipped in Fable, by def name (tools/re-notes/13-hero-gear.md).
// Read-only: nothing here changes the game.
#include <cstdint>
#include <string>
#include <vector>

namespace fo::game {

struct HeroGear {
    std::string melee;   // e.g. OBJECT_SWORD_OF_AEONS; "" for none
    std::string ranged;  // e.g. OBJECT_YEW_LONGBOW; "" for none
    /** Clothing worn now (def names, at most kMaxWorn), as Fable's clothing screen counts it. */
    std::vector<std::string> armour;
    /** False when the clothing list couldn't be read: the report then leaves armour out. */
    bool armourKnown = false;
};

constexpr size_t kMaxWorn = 12;

/**
 * Reads the hero's CTCInventoryWeapons component, and the worn clothing from its
 * CTCHeroAttachableAppearanceModifiers component. False when the hero or the component can't
 * be read (between areas); `out` is then left as it was.
 */
bool read_gear(uintptr_t hero, HeroGear& out);

/** Why read_gear failed last, for the log ("" after a good read). */
const char* gear_missing();

}  // namespace fo::game
