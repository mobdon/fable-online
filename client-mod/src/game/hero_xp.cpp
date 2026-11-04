#include "hero_xp.h"

#include <windows.h>

namespace fo::game {
namespace {

// Component list on every thing: a vector of { int type, component* } (tools/re-notes/12-renown.md).
constexpr uint32_t kComponentsBegin = 0x44;
constexpr uint32_t kComponentsEnd = 0x48;
constexpr size_t kMaxComponents = 64;

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

/** The hero's component with this vtable, or 0. */
uintptr_t component(uintptr_t thing, uint32_t vtable) {
    uint32_t range[2] = {};
    if (!read(thing + kComponentsBegin, range, sizeof range) || range[1] < range[0]) return 0;
    size_t n = (range[1] - range[0]) / 8;
    if (n == 0 || n > kMaxComponents) return 0;
    uint32_t pairs[kMaxComponents * 2];
    if (!read(range[0], pairs, n * 8)) return 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t c = pairs[i * 2 + 1], vt = 0;
        if (c && read(c, &vt, 4) && vt == vtable) return c;
    }
    return 0;
}

/** The data pointer of a vector<int> holding exactly `n` ints, or 0. */
uintptr_t int_vector(uintptr_t at, size_t n) {
    uint32_t v[2] = {};
    if (!read(at, v, sizeof v) || !v[0] || v[1] - v[0] != n * 4) return 0;
    return v[0];
}

struct Parts {
    uintptr_t exp = 0, statXp = 0, statLevels = 0, slots = 0;
};

bool find(uintptr_t hero, Parts& p) {
    if (!hero) return false;
    p.exp = component(hero, kHeroExperienceVtable);
    uintptr_t ab = component(hero, kInventoryAbilitiesVtable);
    if (p.exp) {
        p.statXp = int_vector(p.exp + kStatXp, 3);
        p.statLevels = int_vector(p.exp + kStatLevels, world::kXpStats);
    }
    uint32_t slots = 0;
    if (ab && read(ab + kAbilitySlots, &slots, 4)) p.slots = slots;
    return p.exp && p.statXp && p.statLevels && p.slots;
}

}  // namespace

const char* xp_missing(uintptr_t hero) {
    Parts p;
    if (find(hero, p)) return "";
    if (!p.exp) return "the experience component";
    if (!p.statXp || !p.statLevels) return "the experience component's stat lists";
    return "the abilities component";
}

bool read_xp(uintptr_t hero, world::XpSnapshot& out) {
    Parts p;
    if (!find(hero, p)) return false;
    int32_t head[2], statXp[3], levels[world::kXpStats], slots[world::kXpAbilities * 2];
    if (!read(p.exp + kTotal, head, sizeof head) || !read(p.statXp, statXp, sizeof statXp) ||
        !read(p.statLevels, levels, sizeof levels) || !read(p.slots, slots, sizeof slots))
        return false;
    world::XpSnapshot s;
    s.total = head[0];
    s.pools[0] = head[1];
    for (int i = 0; i < 3; ++i) s.pools[1 + i] = statXp[i];
    for (int i = 0; i < world::kXpPools; ++i)
        if (s.pools[i] < 0 || s.pools[i] > 1000000000) return false;
    for (int i = 0; i < world::kXpStats; ++i) {
        if (levels[i] < 0 || levels[i] > 16) return false;
        s.stats[i] = levels[i];
    }
    for (int i = 0; i < world::kXpAbilities; ++i) {
        int32_t lvl = slots[i * 2];
        if (lvl < -1 || lvl > 7) return false;
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
