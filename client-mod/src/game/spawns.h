#pragma once
// Switching off the local game's own creature spawning.
//
// In a region the server runs, the local game must not create monsters of its own: every
// player would see a different set, and none of them would be the ones the server is
// simulating. Fable spawns them from creature generators placed in the level, so the
// generators' update slot is pointed at a stub while suppression is on.
//
// This changes a vtable in Fable's read-only data, so it is done once, is reversible, and
// is logged. tools/re-notes/05-spawn-and-scripted-control.md has the addresses.
#include <cstddef>

namespace fo::game::spawns {

/** Points the creature generators' update slot at a stub. Safe to call twice. */
bool suppress();
/** Puts the original slots back. */
void restore();
bool suppressed();

/** How many creature generator objects exist right now (for the log). */
size_t count_generators();

}  // namespace fo::game::spawns
