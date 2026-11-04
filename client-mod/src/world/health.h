#pragma once
// Turning the server's health for this player's hero into the number Fable's own hero should
// show. Pure logic, unit-tested in client-mod/tests/world_test.cpp.
//
// The server and Fable size health differently (the server from its own balance table, Fable
// from the hero's Physique), so the server's share of max health is what is kept: a hero at
// half health on the server is at half of Fable's own max locally. Fable's max is left alone.
#include <cstdint>

namespace fo::world {

/** Smaller differences than this (in Fable's units) are not written: rounding, not damage. */
constexpr float kHealthSlack = 0.5f;

/**
 * The health to give the local hero, or a negative number to leave it alone.
 *
 * - No `serverMax` (a sim without combat) or nonsense numbers: leave it alone.
 * - The server says the hero is down (0): 1, never 0. Death on the server is a downed state
 *   with its own revive; letting Fable's own death run would start its game-over sequence.
 * - Otherwise the same share of `fableMax`, at least 1 while the server has the hero alive.
 */
float health_target(float serverHealth, float serverMax, float fableHealth, float fableMax);

}  // namespace fo::world
