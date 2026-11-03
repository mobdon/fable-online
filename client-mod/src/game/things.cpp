#include "things.h"

#include "thing.h"
#include "../log.h"

namespace fo::game::things {
namespace {

Stats g_stats;

const char* what_name(world::ObjectChange::What w) {
    switch (w) {
    case world::ObjectChange::What::DoorOpen: return "open door";
    case world::ObjectChange::What::DoorClosed: return "close door";
    case world::ObjectChange::What::Gone: return "hide item";
    case world::ObjectChange::What::Back: return "show item";
    case world::ObjectChange::What::Opened: return "set opened";
    }
    return "?";
}

}  // namespace

void apply(const world::ObjectChange& change) {
    // Finding the local thing for a server id is the first job in docs/world-objects.md.
    // Until it is mapped, record what should have happened.
    ++g_stats.unmapped;
    if (g_stats.unmapped <= 50) fo::log("things: %s %s (not mapped in the game yet)", what_name(change.what), change.id.c_str());
}

const Stats& stats() { return g_stats; }

bool region_loaded(uint32_t areaBefore) {
    uintptr_t hero = local_hero();
    if (!hero) return false;
    Pos p;
    if (!read_pos(hero, p)) return false;
    uint32_t area = area_id(hero);
    return area != 0 && area != areaBefore;
}

}  // namespace fo::game::things
