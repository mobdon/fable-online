#include "mouse_look.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../config.h"
#include "../log.h"
#include "camera_latch.h"
#include "camera_probe.h"
#include "../game/pause_menu.h"

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

// After applying the mouse, CPCMouseLook's update runs an assist block (0x874152..0x8744C8)
// when the flag at +0x20A of the camera's subject is set: it shifts the focus point a third
// of the way toward a picked target through a spring (offset at +0xE0, eased by 3/4 a tick)
// and turns the yaw at +0xCC toward it, up to 0.375 of a turn. As targets change while the
// mouse turns the view, both pull the camera back and forth. `je 0x8744C9` skips the block
// when the flag is clear; making it a plain jump skips it always, and the spring offset then
// eases back to the hero.
constexpr uint32_t kAssistSite = 0x0087414C, kAssistSkip = 0x008744C9;
constexpr uint8_t kAssistOriginal[2] = {0x0F, 0x84};
char g_assist_status[120] = "camera assist: Fable's own";

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
// CMouseDX (vtable 0x129EA14) slot 5 = 0xAB58E0 reads the cursor offset and warps it back.
constexpr uint32_t kMouseDxVtable = 0x0129EA14, kMouseDxUpdate = 0x00AB58E0;
std::atomic<unsigned> g_mouse_reads{0};
Update_t o_mouse = nullptr;
std::atomic<DWORD> g_last_update_ms{0};

// Per-frame mouse motion, read with GetDeviceState on CMouseDX's DirectInput device
// ([+0x341C]). Fable reads that device's buffered data, which is separate from the
// immediate state, so this does not take anything from the game.
void* g_mouse_dx = nullptr;
int g_state_size = 0;  // 0 = not known yet, -1 = not readable
long g_poll_x = 0, g_poll_y = 0;

void __fastcall hk_mouse(void* self, void*) {
    ++g_mouse_reads;
    g_mouse_dx = self;
    o_mouse(self);
}

// What each camera update did, logged every two seconds while the mouse moves: how far the
// yaw (+0xCC) and pitch (+0xD0) turned and which mouse values (+0x90, +0x94) it used, next
// to the motion read every frame. Calibrates turning the view between updates.
struct CamStats {
    unsigned updates = 0;
    double yaw = 0, pitch = 0, in_x = 0, in_y = 0;
    long mouse_x = 0, mouse_y = 0;
    DWORD since = 0;
    unsigned lines = 0;
} g_cam;

float wrap_turn(float t) {
    while (t > 0.5f) t -= 1.0f;
    while (t < -0.5f) t += 1.0f;
    return t;
}
}  // namespace
void sync_aim();
namespace {

void __fastcall hk_update(void* self, void*) {
    ++g_updates;
    sync_aim();
    fo::game::pause_menu::open();   // keeps the pause screen watch in place
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
    const char* b = static_cast<const char*>(self);
    const float yaw0 = *reinterpret_cast<const float*>(b + 0xCC), pitch0 = *reinterpret_cast<const float*>(b + 0xD0);
    const float in_x = *reinterpret_cast<const float*>(b + 0x90), in_y = *reinterpret_cast<const float*>(b + 0x94);
    o_update(self);
    g_last_update_ms = GetTickCount();
    camera_latch::on_update(wrap_turn(*reinterpret_cast<const float*>(b + 0xCC) - yaw0), reinterpret_cast<const float*>(b + 0x10));
    CamStats& c = g_cam;
    ++c.updates;
    c.yaw += wrap_turn(*reinterpret_cast<const float*>(b + 0xCC) - yaw0);
    c.pitch += *reinterpret_cast<const float*>(b + 0xD0) - pitch0;
    c.in_x += in_x;
    c.in_y += in_y;
    c.mouse_x += g_poll_x;
    c.mouse_y += g_poll_y;
    g_poll_x = g_poll_y = 0;
    const DWORD now = GetTickCount();
    if (!c.since) c.since = now;
    if (now - c.since < 2000) return;
    if ((c.mouse_x || c.mouse_y || c.in_x != 0 || c.in_y != 0) && c.lines < 300) {
        ++c.lines;
        fo::log("camera: %u updates, yaw %+.4f turns, pitch %+.4f, game mouse %+.2f %+.2f, mouse %+ld %+ld counts", c.updates,
                c.yaw, c.pitch, c.in_x, c.in_y, c.mouse_x, c.mouse_y);
        camera_latch::log_stats();
        if (c.lines <= 30) camera_probe::log_counts(c.updates);
        else camera_probe::log_counts(0);
    } else {
        camera_probe::log_counts(0);
    }
    c = CamStats{0, 0, 0, 0, 0, 0, 0, now, c.lines};
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

void install_assist() {
    if (config().camera_assist) return;
    const int32_t rel = *reinterpret_cast<const int32_t*>(kAssistSite + 2);
    if (!readable(kAssistSite, 6) || std::memcmp(reinterpret_cast<const void*>(kAssistSite), kAssistOriginal, 2) != 0 ||
        static_cast<uint32_t>(kAssistSite + 6 + rel) != kAssistSkip) {
        std::snprintf(g_assist_status, sizeof g_assist_status, "camera assist: code not where expected, left as is");
        return;
    }
    const uint32_t jmp = kAssistSkip - (kAssistSite + 5);
    const uint8_t patch[6] = {0xE9, static_cast<uint8_t>(jmp), static_cast<uint8_t>(jmp >> 8), static_cast<uint8_t>(jmp >> 16),
                              static_cast<uint8_t>(jmp >> 24), 0x90};
    write_code(kAssistSite, patch, sizeof patch);
    std::snprintf(g_assist_status, sizeof g_assist_status, "camera assist: off (only the mouse turns the camera)");
}

bool g_aim_ok = false, g_aim_on = false;

// Applies [display] aim_at_crosshair as it is now; called at start and on each camera update
// (game thread, so the targeting search never runs while the bytes change).
void sync_aim() {
    if (!g_aim_ok || config().aim_at_crosshair == g_aim_on) return;
    g_aim_on = config().aim_at_crosshair;
    static const uint8_t patch[3] = {0xB2, 0x01, 0x90};  // mov dl, 1 ; nop
    write_code(kAimSite, g_aim_on ? patch : kAimOriginal, 3);
    std::snprintf(g_aim_status, sizeof g_aim_status, "%s",
                  g_aim_on ? "targeting: along the camera's view (crosshair)" : "targeting: Fable's own (hero's facing)");
}

void install_aim() {
    if (!readable(kAimSite, sizeof kAimOriginal) ||
        std::memcmp(reinterpret_cast<const void*>(kAimSite), kAimOriginal, sizeof kAimOriginal) != 0) {
        std::snprintf(g_aim_status, sizeof g_aim_status, "targeting: code not where expected, left as is");
        return;
    }
    g_aim_ok = true;
    sync_aim();
}

void hook_slot(uint32_t addr, uint32_t expect, void* hook, Update_t& orig) {
    uint32_t* slot = reinterpret_cast<uint32_t*>(addr);
    if (!readable(addr, 4) || *slot != expect) return;
    orig = reinterpret_cast<Update_t>(expect);
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
    *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(hook));
    VirtualProtect(slot, 4, old, &old);
}

void install_counter() {
    hook_slot(kMouseDxVtable + 5 * 4, kMouseDxUpdate, reinterpret_cast<void*>(&hk_mouse), o_mouse);
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
    install_assist();
    camera_probe::install();
    camera_latch::install();
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
const char* assist_status() { return g_assist_status; }

bool in_game() {
    const DWORD t = g_last_update_ms.load();
    return t && GetTickCount() - t < 300;
}

void poll_mouse() {
    void* self = g_mouse_dx;
    if (!self || g_state_size < 0) return;
    const uint32_t dev = *reinterpret_cast<const uint32_t*>(static_cast<const char*>(self) + 0x341C);
    if (!dev || !readable(dev, 4) || !readable(*reinterpret_cast<const uint32_t*>(dev) + 9 * 4, 4)) return;
    using GetDeviceState_t = HRESULT(__stdcall*)(void*, DWORD, void*);
    auto get_state = reinterpret_cast<GetDeviceState_t>((*reinterpret_cast<uint32_t* const*>(dev))[9]);
    struct { LONG x, y, z; BYTE buttons[8]; } st = {};
    DWORD size = g_state_size ? static_cast<DWORD>(g_state_size) : 20;  // DIMOUSESTATE2, else DIMOUSESTATE
    HRESULT hr = get_state(reinterpret_cast<void*>(dev), size, &st);
    if (hr == E_INVALIDARG && !g_state_size) { size = 16; hr = get_state(reinterpret_cast<void*>(dev), size, &st); }
    if (hr == E_INVALIDARG && !g_state_size) {
        g_state_size = -1;
        fo::log("camera: the mouse device's state can't be read");
        return;
    }
    if (FAILED(hr)) return;  // not acquired (menus, alt-tab)
    if (!g_state_size) fo::log("camera: reading mouse motion every frame (%lu-byte state)", static_cast<unsigned long>(size));
    g_state_size = static_cast<int>(size);
    g_poll_x += st.x;
    camera_latch::on_mouse(st.x);
    g_poll_y += st.y;
}
const char* center_status() { return g_center_status; }
const char* aim_status() { return g_aim_status; }
unsigned take_updates() { return g_updates.exchange(0); }
unsigned take_mouse_reads() { return g_mouse_reads.exchange(0); }
}  // namespace fo::display::mouse_look
