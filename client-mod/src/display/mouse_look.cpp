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
char g_center_status[120] = "camera auto-centre: Fable's own";

// While the hero moves (with the matching camera option on), the update eases the pitch
// toward a default by 1/11 of the gap every frame, so at 165 fps it pulls the view back
// about five times faster than at 30. `je 0x874604` skips that block when the option is off.
constexpr uint32_t kCenterSite = 0x0087457A, kCenterSkip = 0x00874604;
constexpr uint8_t kCenterOriginal[2] = {0x0F, 0x84};

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
}  // namespace

void write_code(uint32_t addr, const uint8_t* bytes, size_t n) {
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(addr);
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}

void install_center() {
    if (config().camera_auto_center) return;
    const int32_t rel = *reinterpret_cast<const int32_t*>(kCenterSite + 2);
    if (!readable(kCenterSite, 6) || std::memcmp(reinterpret_cast<const void*>(kCenterSite), kCenterOriginal, 2) != 0 ||
        static_cast<uint32_t>(kCenterSite + 6 + rel) != kCenterSkip) {
        std::snprintf(g_center_status, sizeof g_center_status, "camera auto-centre: code not where expected, left as is");
        return;
    }
    const uint32_t jmp = kCenterSkip - (kCenterSite + 5);
    const uint8_t patch[6] = {0xE9, static_cast<uint8_t>(jmp), static_cast<uint8_t>(jmp >> 8), static_cast<uint8_t>(jmp >> 16),
                              static_cast<uint8_t>(jmp >> 24), 0x90};
    write_code(kCenterSite, patch, sizeof patch);
    std::snprintf(g_center_status, sizeof g_center_status, "camera auto-centre: off");
}

void install() {
    install_center();
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
    write_code(kSite, patch, sizeof patch);
    if (n == 1) std::snprintf(g_status, sizeof g_status, "mouse smoothing: off (camera follows each mouse movement)");
    else std::snprintf(g_status, sizeof g_status, "mouse smoothing: averaged over %u frames", n);
}

const char* status() { return g_status; }
const char* center_status() { return g_center_status; }
}  // namespace fo::display::mouse_look
