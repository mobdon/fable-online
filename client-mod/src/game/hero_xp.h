#pragma once
// The hero's experience in Fable's own memory (tools/re-notes/13-experience.md).
//
// CTCHeroExperience (vtable 0x0125F6C4), one of the hero's components:
//   +0x10 TotalExperiencePoints, +0x14 ExperiencePointsAvailableToSpend (general),
//   +0x18 vector<int> StatExperiencePoints (3: Strength, Skill, Will),
//   +0x24 vector<int> TrainableStatLevels (10).
// CTCInventoryAbilities (vtable 0x01256AD4): +0x160 points at 20 slots of
//   { int level (-1 = not owned, 0 = level 1), CHeroAbilityDef* }.
#include <cstdint>

#include "../world/xp_sync.h"

namespace fo::game {

constexpr uint32_t kHeroExperienceVtable = 0x0125F6C4;
constexpr uint32_t kInventoryAbilitiesVtable = 0x01256AD4;

/** Reads the hero's pools and levels. False until both components are found and look sane. */
bool read_xp(uintptr_t hero, world::XpSnapshot& out);
// The components are found by type in the hero's component list (0x68 experience, 0x6F
// abilities, as Fable's own getters at 0x56FF3D / 0x56FFC5 do), checked against the vtables.
/** Writes pools, total and levels into the hero's components. */
bool write_xp(uintptr_t hero, const world::XpSnapshot& s);
/** For the log: why the last read_xp or write_xp failed. */
const char* xp_why();

}  // namespace fo::game
