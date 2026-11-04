#pragma once
// Fable's own doors, chests and region exits, and the hooks that tell the server about
// them (docs/world-objects.md, "The client side").
//
// Three calls inside Fable.exe are routed through the mod (each is a direct call, so only
// that call site is changed, after checking its bytes):
//   0x7798F6  CTCDRegionExit update -> Trigger (0x779080): the hero walked into an exit
//   0x7507F6  CTCChest update        -> give contents (0x74EE90)
//   0x81235B  container search timer -> give contents (0x74EE90)
//   0x731094 / 0x7310DC  CTCDoor update -> play open (0x730C00)
//   0x7310A4 / 0x731152  CTCDoor update -> play close (0x730DD0)
//   0x5FBD79 / 0x5FC095 / 0x5FC0A8  teleport to a Cullis Gate or the Guild -> CTCTeleporter
//            "move the hero here" (0x7DFCF0)
//   0x41F58D  teleport screen "go back" -> return to where the last seal jump left (0x5FAD40)
// A quest script handing over a container's contents (CGameScriptInterface) is not routed:
// quests run in each player's own game.
//
// Thing ids: the thing manager ([0x13B8A1C], std::map<uint64, CThing*> at +0x58) is keyed
// by the run-time UID, (MapUID << 40) | uid from the level file. The level script name for
// a MapUID comes from the player's own Data/Levels/FinalAlbion.wld.
#include <cstdint>
#include <string>
#include <vector>

#include "../world/held.h"
#include "../world/objects.h"

namespace fo::game::things {

struct Stats {
    uint32_t applied = 0;
    uint32_t unmapped = 0;
    uint32_t exits = 0;
    uint32_t gives = 0;
    uint32_t doors = 0;
    uint32_t teleports = 0;
    uint32_t held = 0;
    uint32_t noId = 0;
};

/** Something the hero did with one of Fable's things, for the server. */
struct Event {
    enum class Kind : uint8_t { Exit, Give, DoorOpened, DoorClosed, Teleport, TeleportBack };
    Kind kind;
    uintptr_t comp;  // the component; the token for a held action
    std::string id;  // the server's thing id ("back" for TeleportBack)
    /** Fable's own action was stopped and waits for release(). */
    bool held = false;
};

/** Reads the level names and routes the call sites above. Safe to call more than once. */
bool install();
void remove();
bool installed();

/**
 * While on, exits and chest gives wait for release(). Off (offline, or the hooks would
 * have no answer), Fable's own action runs at once and the event is only reported.
 */
void set_hold(bool on);
std::vector<Event> drain_events();

/** Runs a held action now. Returns false if the component is gone (area unloaded). */
bool release(uintptr_t comp, world::HeldKind kind);

/**
 * Moves the local hero to a Cullis Gate the way the gate's own travel does (CTCTeleporter's
 * "move the hero here", 0x7DFCF0), so Fable loads that region itself. Called directly, not
 * through the routed call sites, so nothing is reported to the server: the server asked for
 * it. False when the gate's thing is not known to the local game (logged).
 */
bool teleport_to_gate(const std::string& id);

/** Makes the local thing match the server: swing a door. Other changes are logged. */
void apply(const world::ObjectChange& change);
const Stats& stats();
const world::LevelNames& levels();

/**
 * True once the local game has finished a region load after a region change: the hero
 * exists again and is in a different area than before the change. `areaBefore` is
 * game::area_id of the hero when the change began (0 if there was no hero).
 */
bool region_loaded(uint32_t areaBefore);

}  // namespace fo::game::things
