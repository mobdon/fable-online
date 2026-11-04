#include "menu_pages.h"

#include <windows.h>

#include <cstdint>

#include "../log.h"
#include "thing.h"

#define FO_THISCALL __attribute__((thiscall))
#define FO_FASTCALL __attribute__((fastcall))

namespace fo::game::menu_pages {
namespace {

constexpr uintptr_t kRegister = 0x0066EB11;  // void CPageManager::Register(CPage*)
constexpr uintptr_t kSites[] = {0x00577164, 0x00582968, 0x0059C916, 0x005B3710, 0x005C307B,
                                0x005CD365, 0x005F4AAA, 0x00602E51, 0x0061A6CB};
constexpr unsigned kSiteCount = sizeof(kSites) / sizeof(kSites[0]);
constexpr uint32_t kSlots = 0x04;  // vector<CPage*> begin, end at +8

using RegisterFn = void(FO_THISCALL*)(void*, void*);
using IndexFn = int(FO_THISCALL*)(void*);

bool g_patched[kSiteCount] = {};
unsigned g_refused = 0;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}
uint32_t r32(uintptr_t addr) {
    uint32_t v = 0;
    return read(addr, &v, 4) ? v : 0;
}

constexpr uint32_t kPageInComponent = 0x130;      // every caller passes component + 0x130
constexpr uint32_t kPlayerCreature = 0x012457FC;  // CThingPlayerCreature vtable

/** The thing a page belongs to: its CTCInventory* component's thing, at component +4. */
uintptr_t owner_of(uintptr_t page) { return r32(page - kPageInComponent + 4); }

bool is_player_creature(uintptr_t thing) { return thing > 0x10000 && r32(thing) == kPlayerCreature; }

/**
 * True when the page must not reach the menu. Pages of things that aren't the player
 * creature (stand-ins are plain CREATURE_HERO creatures) never register. A second player
 * creature can't replace a page still held by the live local hero either; once that hero
 * is gone (a load or region change) the new hero's pages register as normal.
 */
bool refuse(uintptr_t page, uintptr_t cur, uintptr_t& owner, uintptr_t& had) {
    owner = owner_of(page);
    had = cur ? owner_of(cur) : 0;
    if (!is_player_creature(owner)) return true;
    if (cur && cur != page && had != owner && is_player_creature(had) && had == local_hero()) return true;
    return false;
}

void FO_FASTCALL register_hook(void* mgr, void*, void* page) {
    uintptr_t m = reinterpret_cast<uintptr_t>(mgr), p = reinterpret_cast<uintptr_t>(page);
    if (m && p) {
        int idx = reinterpret_cast<IndexFn>(r32(r32(p) + 8))(page);
        uintptr_t begin = r32(m + kSlots), end = r32(m + kSlots + 4);
        uintptr_t cur = 0;
        if (idx >= 0 && begin && begin + static_cast<uintptr_t>(idx) * 4 < end) cur = r32(begin + idx * 4);
        uintptr_t owner = 0, had = 0;
        if (refuse(p, cur, owner, had)) {
            if (++g_refused <= 20)
                fo::log("menu pages: refused page %d 0x%08X of thing 0x%08X (vtable 0x%08X); slot holds 0x%08X of thing 0x%08X", idx,
                        static_cast<unsigned>(p), static_cast<unsigned>(owner), static_cast<unsigned>(r32(owner)),
                        static_cast<unsigned>(cur), static_cast<unsigned>(had));
            return;
        }
    }
    reinterpret_cast<RegisterFn>(kRegister)(mgr, page);
}

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

}  // namespace

bool install() {
    unsigned done = 0;
    for (unsigned i = 0; i < kSiteCount; ++i) {
        if (!g_patched[i]) g_patched[i] = retarget(kSites[i], kRegister, reinterpret_cast<uintptr_t>(&register_hook));
        if (g_patched[i]) ++done;
    }
    fo::log("menu pages: %u of %u page registrations routed (stand-ins keep off the hero's menu)", done, kSiteCount);
    return done > 0;
}

void remove() {
    for (unsigned i = 0; i < kSiteCount; ++i)
        if (g_patched[i] && retarget(kSites[i], reinterpret_cast<uintptr_t>(&register_hook), kRegister)) g_patched[i] = false;
}

unsigned refused() { return g_refused; }

}  // namespace fo::game::menu_pages
