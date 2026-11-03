#include "spawns.h"

#include <windows.h>

#include <cstdint>

#include "../log.h"
#include "thing.h"

namespace fo::game::spawns {
namespace {

// Vtables of the two generator components, and the slot that runs their spawning work.
// Both take no arguments and return nothing, so a bare `ret` stands in for either.
constexpr uintptr_t kGeneratorVtable = 0x01268C24;         // CTCCreatureGenerator
constexpr uintptr_t kGeneratorCreatorVtable = 0x0126EB3C;  // CTCCreatureGeneratorCreator
constexpr unsigned kUpdateSlot = 0x24;

struct Patched {
    uintptr_t slot = 0;
    uint32_t original = 0;
};

Patched g_patched[2];
bool g_on = false;
unsigned char* g_stub = nullptr;

unsigned char* stub() {
    if (g_stub) return g_stub;
    g_stub = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (g_stub) g_stub[0] = 0xC3;  // ret
    return g_stub;
}

bool patch(uintptr_t slot, uint32_t value, uint32_t& previous) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(slot), 4, PAGE_READWRITE, &old)) return false;
    previous = *reinterpret_cast<uint32_t*>(slot);
    *reinterpret_cast<uint32_t*>(slot) = value;
    VirtualProtect(reinterpret_cast<LPVOID>(slot), 4, old, &old);
    return true;
}

}  // namespace

bool suppressed() { return g_on; }

bool suppress() {
    if (g_on) return true;
    unsigned char* s = stub();
    if (!s) {
        fo::log("spawns: no memory for the stub; local spawning left on");
        return false;
    }
    const uintptr_t vtables[2] = {kGeneratorVtable, kGeneratorCreatorVtable};
    size_t done = 0;
    for (size_t i = 0; i < 2; ++i) {
        uintptr_t slot = vtables[i] + kUpdateSlot;
        uint32_t previous = 0;
        if (!patch(slot, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)), previous)) continue;
        g_patched[done++] = Patched{slot, previous};
    }
    if (done == 0) {
        fo::log("spawns: could not write the generator slots; local spawning left on");
        return false;
    }
    g_on = true;
    fo::log("spawns: local creature spawning off (%u of 2 slots, %u generators in this region)",
            static_cast<unsigned>(done), static_cast<unsigned>(count_generators()));
    return true;
}

void restore() {
    if (!g_on) return;
    for (Patched& p : g_patched) {
        if (!p.slot) continue;
        uint32_t ignored = 0;
        patch(p.slot, p.original, ignored);
        p = Patched{};
    }
    g_on = false;
    fo::log("spawns: local creature spawning back on");
}

size_t count_generators() {
    // The generator objects carry the vtable we patched, so counting them is a plain scan.
    return scan_vtable(static_cast<uint32_t>(kGeneratorVtable), 4096).size();
}

}  // namespace fo::game::spawns
