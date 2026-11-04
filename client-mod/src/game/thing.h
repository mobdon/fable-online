#pragma once
// Reading and writing the live state of things in Fable's own memory: the hero's position
// (tools/re-notes/02-hero-live-state.md) and the same fields on any other creature, which
// is how a puppet is moved without asking the engine's AI for permission.
#include <cstdint>
#include <vector>

namespace fo::game {

struct Pos {
    float x = 0, y = 0, z = 0;
};

/** Component layout, the same for the hero and for every other creature. */
constexpr uint32_t kPhysicsPtr = 0x060;   // thing -> CTCPhysicsControlled
constexpr uint32_t kPosOffset = 0x00C;    // physics -> live x, y, z
constexpr uint32_t kPrevPosOffset = 0x018;  // physics -> position at the last physics step
constexpr uint32_t kForwardOffset = 0x05C;  // physics -> forward x, y
constexpr uint32_t kPrevForwardOffset = 0x074;  // the same, from the last step
constexpr uint32_t kAreaAnchor = 0x0E8;   // thing -> area anchor, changes on an area change

/**
 * Health, on CThing itself (tools/re-notes/11-hero-health.md). Fable's own script getters
 * read these, so they are the hero's real numbers, not a copy for the HUD.
 */
constexpr uint32_t kFlags91 = 0x091;      // bit 0 set: the thing is being removed; no health
constexpr uint32_t kMaxHealth = 0x0B0;    // float
constexpr uint32_t kHealth = 0x0B4;       // float

/** The local hero's CThingPlayerCreature, found by scanning for its vtable. Cached. */
uintptr_t local_hero();
/** Forgets the cached hero: call on a region change. */
void forget_hero();

bool read_pos(uintptr_t thing, Pos& out);
/** Facing in radians, the same convention write_pos uses (forward = -sin, cos). */
bool read_yaw(uintptr_t thing, float& yaw);
/** Writes position and facing straight into the physics component. */
bool write_pos(uintptr_t thing, const Pos& p, bool have_yaw, float yaw);
/** Current and max health. False for a thing being removed, or numbers that make no sense. */
bool read_health(uintptr_t thing, float& health, float& maxHealth);
/**
 * Sets current health straight into the thing, clamped to 0..max. Fable's own death check
 * does not run from here (it runs inside the engine's ModifyHealth), so writing 0 leaves a
 * hero standing at 0: callers keep a live hero at 1 or more.
 */
bool write_health(uintptr_t thing, float health);
/** The thing's physics component, or 0 when it has none yet. */
uintptr_t physics_of(uintptr_t thing);
/** A rough area id: a hash of the hero's area anchor, used until regions are mapped. */
uint32_t area_id(uintptr_t thing);

/** Every live creature object in the process, by vtable. Slow: used by the spawn probe. */
std::vector<uintptr_t> scan_vtable(uint32_t vtable, size_t limit);

/** Vtables used by the mod (fixed: Fable.exe has no ASLR). */
constexpr uint32_t kHeroVtable = 0x012457FC;      // CThingPlayerCreature
constexpr uint32_t kPhysicsVtable = 0x0126616C;   // CTCPhysicsControlled
constexpr uint32_t kPhysicsStandardVtable = 0x01265F84;  // CTCPhysicsStandard
constexpr uint32_t kCreatureVtable = 0x0125AC0C;  // CThingCreatureBase

}  // namespace fo::game
