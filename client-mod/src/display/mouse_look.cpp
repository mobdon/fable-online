#include "mouse_look.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../config.h"
#include "../log.h"

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

// CTCTargetingPlayer's target search (0x804020) scores enemies along a reference direction:
// the hero's facing, or the camera's view direction when the targeting definition's flag at
// +0x3A is set (also used while the first-person targeting button is held). The flag is read
// with `mov dl, [edi+3Ah]`; making that `mov dl, 1` has the search follow the camera.
constexpr uint32_t kAimSite = 0x008040ED;
constexpr uint8_t kAimOriginal[3] = {0x8A, 0x57, 0x3A};
char g_aim_status[120] = "targeting: Fable's own (hero's facing)";

bool readable(uint32_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// CPCMouseLook's vtable; slot 1 (0x873E10) is its per-update camera step. Counting its calls
// shows whether the in-game camera follows the mouse every rendered frame or at a lower rate.
constexpr uint32_t kMouseLookVtable = 0x01277784, kMouseLookUpdate = 0x00873E10;
std::atomic<unsigned> g_updates{0};
using Update_t = void(__thiscall*)(void*);
Update_t o_update = nullptr;
std::atomic<bool> g_chain_logged{false};
void __fastcall hk_update(void* self, void*) {
    ++g_updates;
    if (!g_chain_logged.exchange(true)) {
        // Once: return addresses in Fable.exe on the stack, to trace the loop that runs the camera.
        const uint32_t* sp = reinterpret_cast<const uint32_t*>(__builtin_frame_address(0));
        char buf[600];
        int n = std::snprintf(buf, sizeof buf, "camera: update thread %lu, called from", GetCurrentThreadId());
        for (int i = 0, found = 0; i < 256 && found < 14 && n < 560; ++i) {
            if (!readable(reinterpret_cast<uint32_t>(sp + i), 4)) break;
            const uint32_t v = sp[i];
            if (v > 0x00401000 && v < 0x00E00000) { n += std::snprintf(buf + n, sizeof buf - n, " %08X", v); ++found; }
        }
        fo::log("%s", buf);
    }
    o_update(self);
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

void install_aim() {
    if (!config().aim_at_crosshair) return;
    if (!readable(kAimSite, sizeof kAimOriginal) ||
        std::memcmp(reinterpret_cast<const void*>(kAimSite), kAimOriginal, sizeof kAimOriginal) != 0) {
        std::snprintf(g_aim_status, sizeof g_aim_status, "targeting: code not where expected, left as is");
        return;
    }
    const uint8_t patch[3] = {0xB2, 0x01, 0x90};  // mov dl, 1 ; nop
    write_code(kAimSite, patch, sizeof patch);
    std::snprintf(g_aim_status, sizeof g_aim_status, "targeting: along the camera's view (crosshair)");
}

void install_counter() {
    uint32_t* slot = reinterpret_cast<uint32_t*>(kMouseLookVtable + 4);
    if (!readable(kMouseLookVtable + 4, 4) || *slot != kMouseLookUpdate) return;
    o_update = reinterpret_cast<Update_t>(kMouseLookUpdate);
    const uint32_t hook = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_update));
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
    *slot = hook;
    VirtualProtect(slot, 4, old, &old);
}

void install() {
    install_center();
    install_counter();
    install_aim();
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
const char* aim_status() { return g_aim_status; }
unsigned take_updates() { return g_updates.exchange(0); }
}  // namespace fo::display::mouse_look
