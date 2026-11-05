#pragma once
// Switching off the local game's own creature spawning.
//
// In a region the server runs, the local game must not create monsters of its own: every
// player would see a different set, and none of them would be the ones the server is
// simulating. Fable spawns them from creature generators placed in the level, so the
// generators' update slot is pointed at a stub while suppression is on.
//
// Monsters the level files place directly (the AICreature entries of each .tng) do not come
// from a generator. For those, the calls the level loader makes to build one thing are
// pointed at the mod: a creature whose def the server spawns itself (world::SpawnFilter) is
// removed again straight away, the same way the loader drops a thing it cannot use.
//
// This changes a vtable in Fable's read-only data and five call instructions in its loader,
// all checked before they are touched and logged. tools/re-notes/05-spawn-and-scripted-control.md
// has the addresses.
#include <cstddef>
#include <string>
#include <vector>

namespace fo::game::spawns {

/** Points the creature generators' update slot at a stub. Safe to call twice. */
bool suppress();
/** Puts the original slots back. */
void restore();
bool suppressed();

/** How many creature generator objects exist right now (for the log). */
size_t count_generators();

/**
 * Points the level loader's calls at the mod. Done once, early (before Fable loads a level),
 * and left in place: with no server list the hook only passes calls through.
 */
bool install_level_hook();
/** The creature defs the server spawns itself (from its welcome). Empty: drop nothing. */
void set_server_defs(const std::vector<std::string>& defs);
/** Level-placed creatures removed so far this session, for the log. */
size_t dropped_count();

}  // namespace fo::game::spawns
