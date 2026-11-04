#include "mouse_look.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../config.h"

namespace fo::display::mouse_look {
namespace {
constexpr uint32_t kSite = 0x00873EDC;
// mov esi, [eax+0B4h]   (frames to average, from the camera definition)
constexpr uint8_t kOriginal[6] = {0x8B, 0xB0, 0xB4, 0x00, 0x00, 0x00};
char g_status[120] = "mouse smoothing: Fable's own";

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
}  // namespace

void install() {
    const int frames = config().mouse_smoothing;   // -1 = Fable's own, else frames to average
    if (frames < 0) return;
    if (!readable(kSite, sizeof kOriginal) || std::memcmp(reinterpret_cast<const void*>(kSite), kOriginal, sizeof kOriginal) != 0) {
        std::snprintf(g_status, sizeof g_status, "mouse smoothing: code not where expected, left as is");
        return;
    }
    const uint32_t n = frames < 1 ? 1u : static_cast<uint32_t>(frames);
    // mov esi, n ; nop
    uint8_t patch[6] = {0xBE, static_cast<uint8_t>(n), static_cast<uint8_t>(n >> 8), static_cast<uint8_t>(n >> 16),
                        static_cast<uint8_t>(n >> 24), 0x90};
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(kSite);
    if (!VirtualProtect(at, sizeof patch, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, patch, sizeof patch);
    VirtualProtect(at, sizeof patch, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof patch);
    if (n == 1) std::snprintf(g_status, sizeof g_status, "mouse smoothing: off (camera follows each mouse movement)");
    else std::snprintf(g_status, sizeof g_status, "mouse smoothing: averaged over %u frames", n);
}

const char* status() { return g_status; }
}  // namespace fo::display::mouse_look
