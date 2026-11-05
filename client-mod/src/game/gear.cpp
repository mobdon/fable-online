#include "gear.h"

#include <windows.h>

#include <algorithm>

namespace fo::game {
namespace {

// Things keep their components as {int type, component*} pairs at thing+0x44 (begin) / +0x48 (end).
constexpr uint32_t kComponentsBegin = 0x044;
constexpr uint32_t kFlags91 = 0x091;  // bit 0: the thing is being removed

// CTCInventoryWeapons (save code at 0x005C3A95, vtable 0x01253FF4 slot 1).
constexpr uint32_t kInvWeaponsVtable = 0x01253FF4;
constexpr uint32_t kCompOwner = 0x004;      // component -> its thing
constexpr uint32_t kMeleeHandle = 0x134;    // handle to the active melee weapon thing
constexpr uint32_t kRangedHandle = 0x141;   // handle to the active ranged weapon thing (packed)
constexpr uint32_t kHandleRef = 0x004;      // handle -> ref block; ref block +0 -> thing
constexpr uint32_t kThingDefIndex = 0x08C;  // uint16: the thing's def index

// CTCHeroAttachableAppearanceModifiers (component type 0x5E, vtable 0x01264F0C): +0x30 points at
// an array of {begin, end, cap} vectors by modifier category; category 2 is worn clothing, with
// 8-byte entries whose first dword is the piece's def index. Fable's clothing screen adds up its
// "total armour" from this list (0x005B4A95).
constexpr uint32_t kAppearanceVtable = 0x01264F0C;
constexpr uint32_t kModifierLists = 0x030;
constexpr uint32_t kClothingCategory = 2;

// Def index -> name: entry = [[defs]+0xA4][index]; [entry+4] is the name's offset in the
// symbol table, whose buffer is at [0x13CA83C]; the text starts 4 bytes after the offset.
constexpr uint32_t kDefManager = 0x013B879C;
constexpr uint32_t kDefEntries = 0x0A4;
constexpr uint32_t kEntryName = 0x004;
constexpr uint32_t kSymbolsBegin = 0x013CA83C;
constexpr uint32_t kSymbolsEnd = 0x013CA840;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool u32(uintptr_t addr, uint32_t& v) { return read(addr, &v, 4); }

bool def_name(uint16_t index, std::string& out) {
    uint32_t mgr = 0, entries = 0, entriesEnd = 0, entry = 0, sym = 0, symBegin = 0, symEnd = 0;
    if (!u32(kDefManager, mgr) || !u32(mgr + kDefEntries, entries) || !entries) return false;
    // When +0xA8 looks like the vector's end, keep the index inside it.
    if (u32(mgr + kDefEntries + 4, entriesEnd) && entriesEnd > entries && (entriesEnd - entries) % 4 == 0 &&
        index >= (entriesEnd - entries) / 4)
        return false;
    if (!u32(entries + index * 4u, entry) || !u32(entry + kEntryName, sym) || sym == 0xFFFFFFFFu) return false;
    if (!u32(kSymbolsBegin, symBegin) || !u32(kSymbolsEnd, symEnd) || symEnd <= symBegin) return false;
    if (sym + 4u >= symEnd - symBegin) return false;
    char buf[72] = {};
    const uint32_t at = symBegin + sym + 4;
    const size_t n = symEnd - at < sizeof(buf) - 1 ? symEnd - at : sizeof(buf) - 1;
    if (!read(at, buf, n)) return false;
    buf[sizeof(buf) - 1] = '\0';
    out.assign(buf);
    return !out.empty();
}

/** The def name of the thing a weapon handle points at, "" when the slot is empty. */
bool weapon_at(uintptr_t comp, uint32_t handle, std::string& out) {
    out.clear();
    uint32_t ref = 0, thing = 0;
    if (!u32(comp + handle + kHandleRef, ref)) return false;
    if (!ref) return true;
    if (!u32(ref, thing)) return false;
    if (!thing) return true;
    uint8_t flags = 0;
    uint16_t index = 0;
    if (!read(thing + kFlags91, &flags, 1) || (flags & 1)) return true;
    if (!read(thing + kThingDefIndex, &index, 2) || !index) return false;
    return def_name(index, out);
}

uintptr_t component(uintptr_t hero, uint32_t vtable) {
    uint32_t range[2] = {};
    if (!read(hero + kComponentsBegin, range, sizeof(range))) return 0;
    // The hero carries many components (more than 64): read up to 256 pairs, never give up on a long list.
    if (range[1] < range[0] || (range[1] - range[0]) % 8) return 0;
    static uint32_t pairs[256 * 2];
    const size_t n = std::min<size_t>((range[1] - range[0]) / 8, 256);
    if (!n || !read(range[0], pairs, n * 8)) return 0;
    for (size_t i = 0; i < n; ++i) {
        uint32_t comp = pairs[i * 2 + 1], vt = 0, owner = 0;
        if (comp && u32(comp, vt) && vt == vtable && u32(comp + kCompOwner, owner) && owner == hero) return comp;
    }
    return 0;
}

}  // namespace

bool worn_clothing(uintptr_t hero, std::vector<std::string>& out) {
    out.clear();
    const uintptr_t comp = component(hero, kAppearanceVtable);
    uint32_t lists = 0, range[2] = {};
    if (!comp || !u32(comp + kModifierLists, lists) || !lists) return false;
    if (!read(lists + kClothingCategory * 12, range, sizeof(range))) return false;
    if (range[1] < range[0] || (range[1] - range[0]) % 8 || (range[1] - range[0]) / 8 > 32) return false;
    const size_t n = (range[1] - range[0]) / 8;
    uint32_t items[32 * 2];
    if (n && !read(range[0], items, n * 8)) return false;
    for (size_t i = 0; i < n && out.size() < kMaxWorn; ++i) {
        std::string name;
        if (items[i * 2] && items[i * 2] < 0x10000 && def_name(static_cast<uint16_t>(items[i * 2]), name)) out.push_back(name);
    }
    return true;
}

namespace {
char g_missing[96] = "";
}

const char* gear_missing() { return g_missing; }

bool read_gear(uintptr_t hero, HeroGear& out) {
    uint8_t flags = 0;
    if (!hero || !read(hero + kFlags91, &flags, 1) || (flags & 1)) return false;
    const uintptr_t comp = component(hero, kInvWeaponsVtable);
    if (!comp) {
        uint32_t range[2] = {};
        read(hero + kComponentsBegin, range, sizeof(range));
        wsprintfA(g_missing, "no weapons component among %d", static_cast<int>((range[1] - range[0]) / 8));
        return false;
    }
    HeroGear g;
    if (!weapon_at(comp, kMeleeHandle, g.melee) || !weapon_at(comp, kRangedHandle, g.ranged)) {
        lstrcpyA(g_missing, "weapon handle unreadable");
        return false;
    }
    g_missing[0] = 0;
    // Clothing that can't be read is left out of the report: the server then keeps the ledger's.
    g.armourKnown = worn_clothing(hero, g.armour);
    out = g;
    return true;
}

}  // namespace fo::game
