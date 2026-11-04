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

/** Rises closer together than this are one heal (a potion can land over a few frames). */
constexpr uint32_t kHealGatherMs = 300;
/** After reporting a heal, the local health is not lowered for this long (the server's answer). */
constexpr uint32_t kHealWaitMs = 1500;

/**
 * Notices the local game healing the hero (a potion, food, Heal Life) so the mod can ask the
 * server for it, and holds the local health up while the server answers. Without this the
 * next frame would put the old health straight back, and the heal would be lost.
 */
class HealWatch {
  public:
    /** Call once a frame with Fable's health, before deciding what to write. */
    void observe(float health, float maxHealth, uint32_t nowMs);
    /** Call after the mod itself wrote health, so its own write is not taken for a heal. */
    void wrote(float health) { last_ = health; }
    /** Forget everything (region change, hero rebuilt, offline). */
    void reset() { *this = HealWatch(); }
    /**
     * A heal ready to report: its share of Fable's max. True once per gathered heal.
     */
    bool take_report(uint32_t nowMs, float& share);
    /** True while a reported heal waits for the server: do not lower the local health. */
    bool holding(uint32_t nowMs) const;

  private:
    float last_ = -1;
    float gathered_ = 0;
    uint32_t firstMs_ = 0;
    uint32_t holdUntilMs_ = 0;
    bool gathering_ = false;
};

}  // namespace fo::world
