#include "hero_xp.h"

#include <windows.h>

#include <cstdio>

namespace fo::game {
namespace {

// Component list on every thing: a vector of { int type, component* }, sorted by type
// (tools/re-notes/12-renown.md). Fable fetches these two by type: 0x56FF3D asks for 0x68
// (experience), 0x56FFC5 for 0x6F (abilities). A hero has well over 64 components.
constexpr uint32_t kComponentsBegin = 0x44;
constexpr uint32_t kComponentsEnd = 0x48;
constexpr size_t kMaxComponents = 512;
constexpr uint32_t kHeroExperienceType = 0x68;
constexpr uint32_t kInventoryAbilitiesType = 0x6F;

// CTCHeroExperience
constexpr uint32_t kTotal = 0x10;
constexpr uint32_t kGeneral = 0x14;
constexpr uint32_t kStatXp = 0x18;      // vector<int>: begin, end, capacity
constexpr uint32_t kStatLevels = 0x24;  // vector<int>
// CTCInventoryAbilities
constexpr uint32_t kAbilitySlots = 0x160;
constexpr uint32_t kAbilityStride = 8;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool write(uintptr_t addr, const void* in, size_t n) {
    SIZE_T put = 0;
    return addr > 0x10000 && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), in, n, &put) && put == n;
}

/** Why the last lookup failed, for the log. */
thread_local char g_why[160] = "";

/**
 * The hero's component of this type, checked against its vtable, or 0. Falls back to the
 * vtable alone in case the type id differs in some build.
 */
uintptr_t component(uintptr_t thing, uint32_t type, uint32_t vtable, const char* name) {
    uint32_t range[2] = {};
    if (!read(thing + kComponentsBegin, range, sizeof range) || range[1] < range[0] || (range[1] - range[0]) % 8) {
        std::snprintf(g_why, sizeof g_why, "%s: the component list is unreadable", name);
        return 0;
    }
    size_t n = (range[1] - range[0]) / 8;
    if (n == 0 || n > kMaxComponents) {
        std::snprintf(g_why, sizeof g_why, "%s: the component list has %u entries", name, static_cast<unsigned>(n));
        return 0;
    }
    static uint32_t pairs[kMaxComponents * 2];
    if (!read(range[0], pairs, n * 8)) {
        std::snprintf(g_why, sizeof g_why, "%s: the component list is unreadable", name);
        return 0;
    }
    uint32_t typed = 0, typedVt = 0;
    for (size_t i = 0; i < n; ++i) {
        if (pairs[i * 2] != type) continue;
        typed = pairs[i * 2 + 1];
        if (typed && read(typed, &typedVt, 4) && typedVt == vtable) return typed;
        break;
    }
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = pairs[i * 2 + 1], vt = 0;
        if (c && read(c, &vt, 4) && vt == vtable) return c;
    }
    if (typed)
        std::snprintf(g_why, sizeof g_why, "%s: type 0x%X has vtable %08X, not %08X (%u components)", name, type, typedVt,
                      vtable, static_cast<unsigned>(n));
    else
        std::snprintf(g_why, sizeof g_why, "%s: no type 0x%X among %u components (types %u..%u)", name, type,
                      static_cast<unsigned>(n), pairs[0], pairs[(n - 1) * 2]);
    return 0;
}

/** The data pointer of a vector<int> holding exactly `n` ints, or 0. */
uintptr_t int_vector(uintptr_t at, size_t n, const char* name) {
    uint32_t v[2] = {};
    if (!read(at, v, sizeof v) || !v[0] || v[1] - v[0] != n * 4) {
        std::snprintf(g_why, sizeof g_why, "%s: holds %d ints, expected %u", name,
                      v[0] && v[1] >= v[0] ? static_cast<int>((v[1] - v[0]) / 4) : -1, static_cast<unsigned>(n));
        return 0;
    }
    return v[0];
}

struct Parts {
    uintptr_t exp = 0, statXp = 0, statLevels = 0, slots = 0;
};

bool find(uintptr_t hero, Parts& p) {
    g_why[0] = 0;
    if (!hero) {
        std::snprintf(g_why, sizeof g_why, "no hero");
        return false;
    }
    p.exp = component(hero, kHeroExperienceType, kHeroExperienceVtable, "experience component");
    if (!p.exp) return false;
    p.statXp = int_vector(p.exp + kStatXp, 3, "StatExperiencePoints");
    if (!p.statXp) return false;
    p.statLevels = int_vector(p.exp + kStatLevels, world::kXpStats, "TrainableStatLevels");
    if (!p.statLevels) return false;
    uintptr_t ab = component(hero, kInventoryAbilitiesType, kInventoryAbilitiesVtable, "abilities component");
    if (!ab) return false;
    uint32_t slots = 0;
    if (!read(ab + kAbilitySlots, &slots, 4) || slots < 0x10000) {
        std::snprintf(g_why, sizeof g_why, "abilities component: no slot table at +0x160 (%08X)", slots);
        return false;
    }
    p.slots = slots;
    return true;
}

}  // namespace

const char* xp_why() { return g_why; }

bool read_xp(uintptr_t hero, world::XpSnapshot& out) {
    Parts p;
    if (!find(hero, p)) return false;
    int32_t head[2], statXp[3], levels[world::kXpStats], slots[world::kXpAbilities * 2];
    if (!read(p.exp + kTotal, head, sizeof head) || !read(p.statXp, statXp, sizeof statXp) ||
        !read(p.statLevels, levels, sizeof levels) || !read(p.slots, slots, sizeof slots)) {
        std::snprintf(g_why, sizeof g_why, "the experience values are unreadable");
        return false;
    }
    world::XpSnapshot s;
    s.total = head[0];
    s.pools[0] = head[1];
    for (int i = 0; i < 3; ++i) s.pools[1 + i] = statXp[i];
    for (int i = 0; i < world::kXpPools; ++i)
        if (s.pools[i] < 0 || s.pools[i] > 1000000000) {
            std::snprintf(g_why, sizeof g_why, "experience pool %d is %d", i, s.pools[i]);
            return false;
        }
    for (int i = 0; i < world::kXpStats; ++i) {
        if (levels[i] < 0 || levels[i] > 16) {
            std::snprintf(g_why, sizeof g_why, "stat level %d is %d", i, levels[i]);
            return false;
        }
        s.stats[i] = levels[i];
    }
    for (int i = 0; i < world::kXpAbilities; ++i) {
        int32_t lvl = slots[i * 2];
        if (lvl < -1 || lvl > 7) {
            std::snprintf(g_why, sizeof g_why, "ability slot %d has level %d", i, lvl);
            return false;
        }
        s.abilities[i] = lvl + 1;  // Fable: -1 none, 0 = level 1
    }
    out = s;
    return true;
}

bool write_xp(uintptr_t hero, const world::XpSnapshot& s) {
    Parts p;
    if (!find(hero, p)) return false;
    int32_t head[2] = {s.total, s.pools[0]};
    int32_t statXp[3] = {s.pools[1], s.pools[2], s.pools[3]};
    bool ok = write(p.exp + kTotal, head, sizeof head) && write(p.statXp, statXp, sizeof statXp) &&
              write(p.statLevels, s.stats, sizeof s.stats);
    for (int i = 0; ok && i < world::kXpAbilities; ++i) {
        int32_t lvl = s.abilities[i] - 1;
        int32_t now = 0;
        // Only slots that differ, so an untouched ability is never rewritten.
        if (read(p.slots + i * kAbilityStride, &now, 4) && now != lvl) ok = write(p.slots + i * kAbilityStride, &lvl, 4);
    }
    return ok;
}

}  // namespace fo::game
