#include "things.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "thing.h"
#include "../log.h"

#define FO_THISCALL __attribute__((thiscall))
#define FO_FASTCALL __attribute__((fastcall))

namespace fo::game::things {
namespace {

// ---------------------------------------------------------------- Fable.exe (no ASLR)

constexpr uintptr_t kWorldGlobals = 0x013B8A1C;   // -> object holding the thing map
constexpr uint32_t kThingMap = 0x58;              // STLport map<uint64 uid, CThing*>: header node
constexpr uintptr_t kGameTime = 0x013B89BC;       // game frame counter the doors time from

// Map nodes (STLport _Rb_tree): +4 parent, +8 left, +0xC right, +0x10 key (u64), +0x18 value.
constexpr uint32_t kNodeParent = 0x04, kNodeLeft = 0x08, kNodeRight = 0x0C, kNodeKey = 0x10, kNodeValue = 0x18;

constexpr uint32_t kCompOwner = 0x04;        // CTCBase -> owning CThing
constexpr uint32_t kThingComps = 0x44;       // CThing: sorted vector<{int id, CTCBase*}>, begin/end
constexpr uint32_t kThingPhysics = 0x060;    // CThing -> physics, position at +0x0C

constexpr uint32_t kDoorVtable = 0x01266DE4;      // CTCDoor
constexpr uint32_t kDoorOpen = 0x10;              // bool: wanted state; the update plays the change
constexpr uint32_t kDoorOpenSince = 0x14;         // game time it was last opened (proximity doors)
constexpr uint32_t kExitVtable = 0x0126AB54;      // CTCDRegionExit
constexpr uint32_t kChestVtable = 0x0126957C;     // CTCChest
constexpr uint32_t kRewardVtable = 0x01269424;    // CTCContainerRewardHero
constexpr uint32_t kSearchVtable = 0x012431F4;    // CTCSearchableContainer

constexpr uintptr_t kExitTrigger = 0x00779080;    // void CTCDRegionExit::Trigger()
constexpr uintptr_t kGiveContents = 0x0074EE90;   // bool CTCContainerRewardHero::GiveToHero()
constexpr uintptr_t kDoorPlayOpen = 0x00730C00;   // void CTCDoor::PlayOpen(bool animate)
constexpr uintptr_t kDoorPlayClose = 0x00730DD0;  // void CTCDoor::PlayClose()

/** A door the hero used is reported only if the hero is this close (villagers open doors too). */
constexpr float kDoorReportRange = 5.0f;

using ExitTriggerFn = void(FO_THISCALL*)(void*);
using GiveFn = uint8_t(FO_THISCALL*)(void*);
using DoorOpenFn = void(FO_THISCALL*)(void*, int);
using DoorCloseFn = void(FO_THISCALL*)(void*);

Stats g_stats;
world::LevelNames g_levels;
bool g_installed = false;
bool g_hold = false;
std::vector<Event> g_events;
/** Things the server just set, so their own open/close is not reported back. */
std::unordered_map<uintptr_t, bool> g_applying;
/** Owner thing -> run-time UID, filled by walking the thing map. */
std::unordered_map<uintptr_t, uint64_t> g_uidOf;
DWORD g_lastWalk = 0;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
uint32_t r32(uintptr_t addr) {
    uint32_t v = 0;
    return read(addr, &v, 4) ? v : 0;
}
bool w8(uintptr_t addr, uint8_t v) {
    SIZE_T put = 0;
    return addr > 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), &v, 1, &put) && put == 1;
}
bool w32(uintptr_t addr, uint32_t v) {
    SIZE_T put = 0;
    return addr > 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), &v, 4, &put) && put == 4;
}

uintptr_t map_header() {
    uintptr_t mgr = r32(kWorldGlobals);
    return mgr ? r32(mgr + kThingMap) : 0;
}

/** The thing with this run-time UID, or 0. */
uintptr_t find_thing(uint64_t uid) {
    uintptr_t node = r32(map_header() + kNodeParent);  // the root
    for (int depth = 0; node && depth < 64; ++depth) {
        uint64_t key = 0;
        if (!read(node + kNodeKey, &key, 8)) return 0;
        if (key == uid) return r32(node + kNodeValue);
        node = r32(node + (uid < key ? kNodeLeft : kNodeRight));
    }
    return 0;
}

/** Walks the whole thing map once, filling g_uidOf. Rate-limited: it reads every node. */
void walk_things() {
    if (GetTickCount() - g_lastWalk < 500) return;
    g_lastWalk = GetTickCount();
    g_uidOf.clear();
    std::vector<uintptr_t> stack;
    uintptr_t root = r32(map_header() + kNodeParent);
    if (root) stack.push_back(root);
    size_t seen = 0;
    while (!stack.empty() && seen < 400000) {
        uintptr_t n = stack.back();
        stack.pop_back();
        ++seen;
        struct { uint32_t color, parent, left, right; uint64_t key; uint32_t value; } node;
        static_assert(sizeof(node) == 0x1C + 4, "node layout");
        if (!read(n, &node, 0x1C)) continue;
        if (node.value) g_uidOf[node.value] = node.key;
        if (node.left) stack.push_back(node.left);
        if (node.right) stack.push_back(node.right);
    }
    static bool logged = false;
    if (!logged) {
        logged = true;
        fo::log("things: thing map holds %u things", static_cast<unsigned>(g_uidOf.size()));
    }
}

/** The run-time UID of a thing, or 0. */
uint64_t uid_of_thing(uintptr_t thing) {
    auto it = g_uidOf.find(thing);
    if (it != g_uidOf.end() && find_thing(it->second) == thing) return it->second;
    walk_things();
    it = g_uidOf.find(thing);
    return it != g_uidOf.end() ? it->second : 0;
}

std::string id_of_component(uintptr_t comp) {
    uintptr_t thing = r32(comp + kCompOwner);
    uint64_t uid = thing ? uid_of_thing(thing) : 0;
    std::string id = uid ? world::server_thing_id(uid, g_levels) : std::string();
    if (id.empty()) {
        ++g_stats.noId;
        if (g_stats.noId <= 20)
            fo::log("things: no server id for component 0x%08X (thing 0x%08X, uid %08X%08X)", static_cast<unsigned>(comp),
                    static_cast<unsigned>(thing), static_cast<unsigned>(uid >> 32), static_cast<unsigned>(uid));
    }
    return id;
}

/** The component of `thing` whose vtable is `vtable`, or 0. */
uintptr_t component(uintptr_t thing, uint32_t vtable) {
    uintptr_t begin = r32(thing + kThingComps), end = r32(thing + kThingComps + 4);
    if (!begin || end < begin || end - begin > 8 * 512) return 0;
    for (uintptr_t p = begin; p < end; p += 8) {
        uintptr_t c = r32(p + 4);
        if (c && r32(c) == vtable) return c;
    }
    return 0;
}

bool thing_pos(uintptr_t thing, float out[3]) {
    uintptr_t phys = r32(thing + kThingPhysics);
    if (!phys || !read(phys + 0x0C, out, 12)) return false;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(out[i]) || std::fabs(out[i]) > 1e6f) return false;
    return true;
}

bool hero_near(uintptr_t thing, float range) {
    uintptr_t hero = local_hero();
    Pos h;
    float t[3];
    if (!hero || !read_pos(hero, h) || !thing_pos(thing, t)) return false;
    return std::hypot(h.x - t[0], h.y - t[1]) <= range && std::fabs(h.z - t[2]) <= 3.0f;
}

// ---------------------------------------------------------------- the routed calls

void FO_FASTCALL exit_trigger_hook(void* self, void*) {
    uintptr_t comp = reinterpret_cast<uintptr_t>(self);
    ++g_stats.exits;
    std::string id = id_of_component(comp);
    if (g_hold && !id.empty()) {
        ++g_stats.held;
        g_events.push_back(Event{Event::Kind::Exit, comp, id, true});
        return;  // the region load starts when the server moves the hero (release)
    }
    if (!id.empty()) g_events.push_back(Event{Event::Kind::Exit, comp, id, false});
    reinterpret_cast<ExitTriggerFn>(kExitTrigger)(self);
}

uint8_t FO_FASTCALL give_hook(void* self, void*) {
    uintptr_t comp = reinterpret_cast<uintptr_t>(self);
    ++g_stats.gives;
    std::string id = id_of_component(comp);
    if (g_hold && !id.empty()) {
        ++g_stats.held;
        g_events.push_back(Event{Event::Kind::Give, comp, id, true});
        return 1;  // "gave something": the game shows no "nothing here" sign while it waits
    }
    if (!id.empty()) g_events.push_back(Event{Event::Kind::Give, comp, id, false});
    return reinterpret_cast<GiveFn>(kGiveContents)(self);
}

void door_changed(uintptr_t door, bool open) {
    auto it = g_applying.find(door);
    if (it != g_applying.end()) {
        bool fromServer = it->second == open;
        g_applying.erase(it);
        if (fromServer) return;  // the server's own change, nothing to report
    }
    uintptr_t thing = r32(door + kCompOwner);
    if (!thing || !hero_near(thing, kDoorReportRange)) return;
    std::string id = id_of_component(door);
    if (id.empty()) return;
    ++g_stats.doors;
    g_events.push_back(Event{open ? Event::Kind::DoorOpened : Event::Kind::DoorClosed, door, id, false});
}

void FO_FASTCALL door_open_hook(void* self, void*, int animate) {
    reinterpret_cast<DoorOpenFn>(kDoorPlayOpen)(self, animate);
    door_changed(reinterpret_cast<uintptr_t>(self), true);
}

void FO_FASTCALL door_close_hook(void* self, void*) {
    reinterpret_cast<DoorCloseFn>(kDoorPlayClose)(self);
    door_changed(reinterpret_cast<uintptr_t>(self), false);
}

struct CallSite {
    uintptr_t at;
    uintptr_t target;  // what the call goes to in the retail exe
    void* hook;
    bool patched;
};

CallSite g_sites[] = {
    {0x007798F6, kExitTrigger, reinterpret_cast<void*>(&exit_trigger_hook), false},
    {0x007507F6, kGiveContents, reinterpret_cast<void*>(&give_hook), false},
    {0x0081235B, kGiveContents, reinterpret_cast<void*>(&give_hook), false},
    {0x00731094, kDoorPlayOpen, reinterpret_cast<void*>(&door_open_hook), false},
    {0x007310DC, kDoorPlayOpen, reinterpret_cast<void*>(&door_open_hook), false},
    {0x007310A4, kDoorPlayClose, reinterpret_cast<void*>(&door_close_hook), false},
    {0x00731152, kDoorPlayClose, reinterpret_cast<void*>(&door_close_hook), false},
};

/** Points the call at `site.at` to `to`, if it still calls `from` (so another exe is left alone). */
bool retarget(uintptr_t at, uintptr_t from, uintptr_t to) {
    uint8_t op = 0;
    int32_t rel = 0;
    if (!read(at, &op, 1) || !read(at + 1, &rel, 4)) return false;
    if (op != 0xE8 || at + 5 + rel != from) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(at + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    *reinterpret_cast<int32_t*>(at + 1) = static_cast<int32_t>(to - (at + 5));
    VirtualProtect(reinterpret_cast<LPVOID>(at + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(at), 5);
    return true;
}

bool load_levels() {
    char path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return false;
    std::string dir(path, n);
    dir.erase(dir.find_last_of("\\/") + 1);
    std::string file = dir + "Data\\Levels\\FinalAlbion.wld";
    FILE* f = std::fopen(file.c_str(), "rb");
    if (!f) {
        fo::log("things: can't read %s; doors, chests and exits stay local", file.c_str());
        return false;
    }
    std::string text;
    char buf[65536];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < (16u << 20)) text.append(buf, got);
    std::fclose(f);
    g_levels = world::parse_wld_levels(text);
    fo::log("things: %u levels named from FinalAlbion.wld", static_cast<unsigned>(g_levels.size()));
    return !g_levels.empty();
}

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

bool install() {
    if (g_installed) return true;
    if (!load_levels()) return false;
    unsigned done = 0;
    for (CallSite& s : g_sites) {
        s.patched = retarget(s.at, s.target, reinterpret_cast<uintptr_t>(s.hook));
        if (s.patched) ++done;
        else fo::log("things: call at 0x%08X is not what this mod expects; left alone", static_cast<unsigned>(s.at));
    }
    g_installed = done > 0;
    fo::log("things: %u of %u Fable calls routed (exit, chest and search gives, doors)", done,
            static_cast<unsigned>(sizeof(g_sites) / sizeof(g_sites[0])));
    return g_installed;
}

void remove() {
    for (CallSite& s : g_sites)
        if (s.patched && retarget(s.at, reinterpret_cast<uintptr_t>(s.hook), s.target)) s.patched = false;
    g_installed = false;
    g_hold = false;
}

bool installed() { return g_installed; }

void set_hold(bool on) { g_hold = on && g_installed; }

std::vector<Event> drain_events() {
    std::vector<Event> out;
    out.swap(g_events);
    return out;
}

bool release(uintptr_t comp, world::HeldKind kind) {
    uint32_t vt = r32(comp);
    if (kind == world::HeldKind::Exit) {
        if (vt != kExitVtable) return false;
        reinterpret_cast<ExitTriggerFn>(kExitTrigger)(reinterpret_cast<void*>(comp));
        return true;
    }
    if (vt != kChestVtable && vt != kRewardVtable && vt != kSearchVtable) return false;
    reinterpret_cast<GiveFn>(kGiveContents)(reinterpret_cast<void*>(comp));
    return true;
}

void apply(const world::ObjectChange& change) {
    using What = world::ObjectChange::What;
    if (change.what == What::DoorOpen || change.what == What::DoorClosed) {
        uint64_t uid = world::runtime_uid(change.id, g_levels);
        uintptr_t thing = uid ? find_thing(uid) : 0;
        uintptr_t door = thing ? component(thing, kDoorVtable) : 0;
        if (door) {
            bool open = change.what == What::DoorOpen;
            uint8_t now = 0;
            read(door + kDoorOpen, &now, 1);
            if ((now != 0) != open) {
                // The door's own update sees the wanted state change and plays it. A
                // proximity door keeps it for its usual delay, counted from now.
                g_applying[door] = open;
                w8(door + kDoorOpen, open ? 1 : 0);
                if (open) w32(door + kDoorOpenSince, r32(kGameTime));
            }
            ++g_stats.applied;
            return;
        }
    }
    // Hiding items and marking chests opened are not mapped yet; the server's ledger already
    // stops a second give, since every give waits for its answer.
    ++g_stats.unmapped;
    if (g_stats.unmapped <= 50) fo::log("things: %s %s (not applied in the game)", what_name(change.what), change.id.c_str());
}

const Stats& stats() { return g_stats; }
const world::LevelNames& levels() { return g_levels; }

bool region_loaded(uint32_t areaBefore) {
    uintptr_t hero = local_hero();
    if (!hero) return false;
    Pos p;
    if (!read_pos(hero, p)) return false;
    uint32_t area = area_id(hero);
    return area != 0 && area != areaBefore;
}

}  // namespace fo::game::things
