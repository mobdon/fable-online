#include "player_modes.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include "../log.h"

namespace fo::game::player_modes {
namespace {
constexpr uint32_t kTable = 0x013B8C88, kStride = 0x20, kLeaveOffset = 0x10;
constexpr int kModes = 64, kPauseMode = 23;   // 23 belongs to pause_menu
constexpr uint32_t kPauseEnter = 0x00636270;
constexpr uint32_t kCodeLo = 0x00401000, kCodeHi = 0x01200000;

std::atomic<uint64_t> g_mask{0};
std::atomic<int> g_logged{0};
std::atomic<bool> g_done{false};
DWORD g_next_try = 0;

void __stdcall note(int id, int started) {
    if (id < 0 || id >= kModes) return;
    const uint64_t bit = 1ull << id;
    if (started) g_mask |= bit;
    else g_mask &= ~bit;
    if (g_logged < 400) {
        ++g_logged;
        fo::log("player mode %d %s", id, started ? "started" : "ended");
    }
}

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
uint32_t word(uint32_t a) { return *reinterpret_cast<volatile const uint32_t*>(a); }
bool code(uint32_t v) { return v >= kCodeLo && v < kCodeHi; }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// pushad ; push started ; push id ; call note ; popad ; jmp target  (20 bytes)
void thunk(uint8_t* p, int id, int started, uint32_t target) {
    *p++ = 0x60;
    *p++ = 0x6A; *p++ = static_cast<uint8_t>(started);
    *p++ = 0x6A; *p++ = static_cast<uint8_t>(id);
    *p++ = 0xE8;
    put32(p, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&note)) - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) + 4));
    p += 4;
    *p++ = 0x61;
    *p++ = 0xE9;
    put32(p, target - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) + 4));
}

void patch() {
    if (!readable(kTable, kModes * kStride)) return;
    // The table is filled by a static initialiser: wait until the pause entry is there (Fable's
    // own handler, or pause_menu's thunk once it has swapped it).
    const uint32_t pause = word(kTable + kPauseMode * kStride);
    if (pause != kPauseEnter && code(pause)) return;
    if (!pause) return;
    auto* cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, kModes * 2 * 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!cave) { g_done = true; return; }
    int patched = 0;
    for (int id = 0; id < kModes; ++id) {
        if (id == kPauseMode) continue;
        const uint32_t enter_at = kTable + id * kStride, leave_at = enter_at + kLeaveOffset;
        const uint32_t enter = word(enter_at), leave = word(leave_at);
        if (!code(enter) || !code(leave)) continue;
        uint8_t* e = cave + id * 64;
        thunk(e, id, 1, enter);
        thunk(e + 32, id, 0, leave);
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(enter_at), kStride, PAGE_READWRITE, &old)) continue;
        *reinterpret_cast<volatile uint32_t*>(enter_at) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e));
        *reinterpret_cast<volatile uint32_t*>(leave_at) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e + 32));
        VirtualProtect(reinterpret_cast<void*>(enter_at), kStride, old, &old);
        ++patched;
    }
    FlushInstructionCache(GetCurrentProcess(), cave, kModes * 2 * 32);
    g_done = true;
    fo::log("player modes: watching %d mode handlers", patched);
}
}  // namespace

void poll() {
    if (g_done) return;
    const DWORD now = GetTickCount();
    if (now < g_next_try) return;
    g_next_try = now + 500;
    patch();
}

bool running(int id) { return id >= 0 && id < kModes && (g_mask.load() >> id) & 1; }
uint64_t running_mask() { return g_mask.load(); }
}  // namespace fo::game::player_modes
