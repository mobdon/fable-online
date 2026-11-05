#include "pause_menu.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../log.h"

namespace fo::game::pause_menu {
namespace {
constexpr uint32_t kEnterSlot = 0x013B8C88 + 23 * 0x20;   // 0x013B8F68
constexpr uint32_t kLeaveSlot = 0x013B8C98 + 23 * 0x20;   // 0x013B8F78
constexpr uint32_t kEnter = 0x00636270, kLeave = 0x006362D0;

std::atomic<bool> g_open{false};
std::atomic<bool> g_patched{false};
std::atomic<int> g_logged{0};
char g_status[100] = "pause menu: not watched yet";
uint8_t* g_cave = nullptr;

void __stdcall note(int opened) {
    g_open = opened != 0;
    if (g_logged < 100) {
        ++g_logged;
        fo::log("pause menu: %s", opened ? "open" : "closed");
    }
}

bool readable_word(uint32_t addr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + 4 <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

uint32_t word(uint32_t addr) { return *reinterpret_cast<volatile const uint32_t*>(addr); }
void put32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// One thunk: pushad ; push flag ; call note ; popad ; jmp target. Registers and the stack
// (this in ecx, Fable's two arguments) reach Fable's handler as they came in.
uint8_t* thunk(uint8_t* p, int flag, uint32_t target) {
    *p++ = 0x60;                                   // pushad
    *p++ = 0x6A; *p++ = static_cast<uint8_t>(flag); // push flag
    *p++ = 0xE8;                                   // call note
    put32(p, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&note)) - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) + 4));
    p += 4;
    *p++ = 0x61;                                   // popad
    *p++ = 0xE9;                                   // jmp target
    put32(p, target - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)) + 4));
    return p + 4;
}

bool try_patch(bool can_log) {
    // Fable's static initialiser may run after the DLL is loaded and put its handlers back.
    if (g_patched && word(kEnterSlot) == kEnter) g_patched = false;
    if (g_patched) return true;
    if (!readable_word(kEnterSlot) || !readable_word(kLeaveSlot)) return false;
    if (word(kEnterSlot) != kEnter || word(kLeaveSlot) != kLeave) return false;   // not filled yet
    if (!g_cave) {
        g_cave = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!g_cave) return false;
        thunk(g_cave, 1, kEnter);
        thunk(g_cave + 32, 0, kLeave);
        FlushInstructionCache(GetCurrentProcess(), g_cave, 64);
    }
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(kEnterSlot);
    if (!VirtualProtect(at, 0x14, PAGE_READWRITE, &old)) return false;
    *reinterpret_cast<volatile uint32_t*>(kEnterSlot) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cave));
    *reinterpret_cast<volatile uint32_t*>(kLeaveSlot) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_cave + 32));
    VirtualProtect(at, 0x14, old, &old);
    g_patched = true;
    std::snprintf(g_status, sizeof g_status, "pause menu: watched (mode 23 handlers)");
    if (can_log) fo::log("%s", g_status);
    return true;
}
}  // namespace

void install() {
    if (!try_patch(false))   // DllMain: no logging under the loader lock
        std::snprintf(g_status, sizeof g_status, "pause menu: handler table not filled yet, retrying each frame");
}

bool open() {
    try_patch(true);
    return g_open;
}

const char* status() { return g_status; }
}  // namespace fo::game::pause_menu
