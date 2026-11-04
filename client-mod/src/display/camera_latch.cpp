#include "camera_latch.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../config.h"
#include "../log.h"

namespace fo::display::camera_latch {
namespace {
// CEngine's vtable slot 4: SetCamera(desc), 0x00B23B50. desc: position +0x00, forward +0x0C,
// up +0x18 (floats, world space, Z up). The engine builds the frame's view from it.
constexpr uint32_t kSetCameraSlot = 0x012A0F4C, kSetCamera = 0x00B23B50;
using SetCamera_t = void(__thiscall*)(void*, float*);
SetCamera_t o_set_camera = nullptr;

constexpr double kTwoPi = 6.283185307179586;
constexpr double kTickSeconds = 1.0 / 15.0;

char g_status[160] = "camera follow every frame: not installed";

LARGE_INTEGER g_freq = {};
double now_s() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    return static_cast<double>(t.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

struct State {
    // Latest mouse-look update.
    double update_at = 0;
    float last_turn = 0;          // yaw turn of the last update (turns), for the log
    float focus[3] = {};
    long counts_since = 0;        // mouse counts since that update
    long consumed = 0;            // mouse counts the last update turned by
    // Fit of the view's heading jump at the first frame after each update (radians) against
    // the mouse counts that update used: radians per count, sign included.
    double s_jc = 0, s_cc = 0, s_jj = 0;
    unsigned fit_samples = 0;
    // Learning: yaw turns per mouse count, over windows with enough motion.
    double win_turn = 0, win_counts = 0;
    double turns_per_count = 0;
    int k_windows = 0;
    // Which way the rendered view turns for a positive Fable yaw, from the view direction seen
    // at SetCamera across updates.
    bool have_heading = false;
    double last_heading = 0;
    // Whether frames between updates show the view moving (blending) or holding still.
    unsigned moved_between = 0, still_between = 0;
    bool updated_since_frame = false;
    // Stats.
    unsigned frames = 0, turned = 0;
    double max_lead = 0;
} g;

double heading(const float* fwd) { return std::atan2(static_cast<double>(fwd[1]), static_cast<double>(fwd[0])); }
double wrap_pi(double a) {
    while (a > 3.141592653589793) a -= kTwoPi;
    while (a < -3.141592653589793) a += kTwoPi;
    return a;
}

void rotate_z(float* v, double c, double s) {
    const double x = v[0], y = v[1];
    v[0] = static_cast<float>(c * x - s * y);
    v[1] = static_cast<float>(s * x + c * y);
}

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

// Radians of heading per mouse count, or 0 while the fit isn't good enough to trust.
double rad_per_count() {
    if (g.fit_samples < 20 || g.s_cc < 2000 || g.s_jj <= 0) return 0;
    const double k = g.s_jc / g.s_cc;
    const double r2 = g.s_jc * g.s_jc / (g.s_cc * g.s_jj);
    return r2 >= 0.6 ? k : 0;
}

// The view's lead over what Fable shows, in radians of heading. Fable holds the view still
// between updates, so the lead is the mouse motion since the last update; the next update
// then turns the view by that much and the lead starts again from zero.
double lead(double now) {
    if (now - g.update_at > 0.25) return 0;  // mouse-look isn't running (menus, cut scenes)
    if (g.moved_between > 2 * g.still_between) return 0;  // the view blends: not handled
    double r = rad_per_count() * static_cast<double>(g.counts_since);
    if (r > 0.6) r = 0.6;
    if (r < -0.6) r = -0.6;
    return r;
}

// Smoothing. Fable sets its camera every frame, but the heading only steps when the game's
// camera update runs (15 times a second), and the camera's height follows the hero's feet, so
// every bump in the ground under the hero shakes the view up and down. Each frame here eases
// the camera toward Fable's: the direction over kDirTau (hides the turn steps), the position
// across the ground over kFlatTau, and the height over kHeightTau, long enough that small bumps
// in the ground don't reach the view while hills and stairs still do.
constexpr double kDirTau = 0.05, kFlatTau = 0.03, kHeightTau = 0.25;
struct Smooth {
    bool have = false;
    float shown[9] = {};
    double at = 0;
    unsigned frames = 0, snaps = 0;
    double raw_dz = 0, shown_dz = 0;   // summed frame-to-frame height change, Fable's vs shown
    float last_raw_z = 0;
} sm;

void normalize(float* v) {
    const double l = std::sqrt(static_cast<double>(v[0]) * v[0] + static_cast<double>(v[1]) * v[1] + static_cast<double>(v[2]) * v[2]);
    if (l > 1e-6) for (int i = 0; i < 3; ++i) v[i] = static_cast<float>(v[i] / l);
}

void ease(float* v, const float* to, double k) {
    *v = static_cast<float>(*v + (*to - *v) * k);
}

// Writes the camera to show this frame into out[9]; false = show Fable's as it is.
bool smooth(const float* desc, double now, float* out) {
    if (sm.have) {
        const double dt = now - sm.at;
        const double dx = desc[0] - sm.shown[0], dy = desc[1] - sm.shown[1], dz = desc[2] - sm.shown[2];
        const double dot = static_cast<double>(desc[3]) * sm.shown[3] + static_cast<double>(desc[4]) * sm.shown[4] +
                           static_cast<double>(desc[5]) * sm.shown[5];
        if (dt > 0.3 || dt < 0 || dx * dx + dy * dy + dz * dz > 6.0 * 6.0 || dot < 0.5) {
            sm.have = false;   // a cut, a teleport or a pause: jump straight there
            ++sm.snaps;
        }
    }
    if (!sm.have) {
        std::memcpy(sm.shown, desc, sizeof sm.shown);
        sm.at = now;
        sm.last_raw_z = desc[2];
        sm.have = true;
        return false;
    }
    const double dt = now - sm.at;
    sm.at = now;
    const double kd = 1.0 - std::exp(-dt / kDirTau), kf = 1.0 - std::exp(-dt / kFlatTau), kh = 1.0 - std::exp(-dt / kHeightTau);
    const float old_z = sm.shown[2];
    ease(&sm.shown[0], &desc[0], kf);
    ease(&sm.shown[1], &desc[1], kf);
    ease(&sm.shown[2], &desc[2], kh);
    for (int i = 3; i < 9; ++i) ease(&sm.shown[i], &desc[i], kd);
    normalize(sm.shown + 3);
    normalize(sm.shown + 6);
    sm.raw_dz += std::fabs(desc[2] - sm.last_raw_z);
    sm.shown_dz += std::fabs(sm.shown[2] - old_z);
    sm.last_raw_z = desc[2];
    std::memcpy(out, sm.shown, sizeof sm.shown);
    ++sm.frames;
    return true;
}

// Which shoulder: config sign, flipped with the swap key.
bool g_swapped = false, g_key_down = false;
HWND g_game_window = nullptr;
unsigned g_shoulder_frames = 0;

float shoulder_now() {
    const float base = config().camera_shoulder;
    if (base == 0.0f) return 0.0f;
    const int key = config().camera_shoulder_key;
    if (key > 0) {
        const bool down = (GetAsyncKeyState(key) & 0x8000) != 0;
        if (down && !g_key_down) {
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid == GetCurrentProcessId()) {
                g_swapped = !g_swapped;
                fo::log("camera: over the %s shoulder", (base < 0) != g_swapped ? "left" : "right");
            }
        }
        g_key_down = down;
    }
    return g_swapped ? -base : base;
}

void __fastcall hk_set_camera(void* self, void*, float* desc) {
    if (!desc || !readable(desc, 0x24)) { o_set_camera(self, desc); return; }
    const double now = now_s();
    const double h = heading(desc + 3);
    if (g.have_heading) {
        const double d = wrap_pi(h - g.last_heading);
        if (!g.updated_since_frame) {
            if (std::fabs(d) > 1e-6) ++g.moved_between; else ++g.still_between;
        } else if (g.consumed != 0 && std::fabs(d) < 0.8) {
            const double c = static_cast<double>(g.consumed);
            g.s_jc = 0.995 * g.s_jc + d * c;
            g.s_cc = 0.995 * g.s_cc + c * c;
            g.s_jj = 0.995 * g.s_jj + d * d;
            ++g.fit_samples;
        }
    }
    g.have_heading = true;
    g.last_heading = h;
    g.updated_since_frame = false;
    ++g.frames;

    const double dx = desc[0] - g.focus[0], dy = desc[1] - g.focus[1], dz = desc[2] - g.focus[2];
    const bool near_hero = dx * dx + dy * dy + dz * dz < 40.0 * 40.0;  // the mouse-look camera, not a cut scene's
    const bool mouse_look = near_hero && now - g.update_at < 0.3;
    float saved[9];
    std::memcpy(saved, desc, sizeof saved);
    bool changed = false;
    if (mouse_look && config().camera_smooth) {
        float out[9];
        if (smooth(desc, now, out)) { std::memcpy(desc, out, sizeof out); changed = true; }
    } else {
        sm.have = false;
    }
    const double r = config().camera_latch && !config().camera_smooth && mouse_look ? lead(now) : 0.0;
    const float shoulder = mouse_look ? shoulder_now() : 0.0f;
    if (std::fabs(r) >= 1e-5) {
        const double c = std::cos(r), s = std::sin(r);
        float off[3] = {desc[0] - g.focus[0], desc[1] - g.focus[1], desc[2] - g.focus[2]};
        rotate_z(off, c, s);
        desc[0] = g.focus[0] + off[0];
        desc[1] = g.focus[1] + off[1];
        rotate_z(desc + 3, c, s);
        rotate_z(desc + 6, c, s);
        ++g.turned;
        if (std::fabs(r) > g.max_lead) g.max_lead = std::fabs(r);
        changed = true;
    }
    if (shoulder != 0.0f) {
        // Over the shoulder: move the camera sideways, level, keeping its direction, so the
        // hero sits off the centre of the view. Positive = the hero's right, negative = left.
        const double fx = desc[3], fy = desc[4], len = std::sqrt(fx * fx + fy * fy);
        if (len > 1e-3) {
            // Right of the view, level. Fable's world is right-handed with Z up: the engine
            // camera's view x axis comes out as (fy, -fx) for a level forward (fx, fy).
            const double rx = fy / len, ry = -fx / len;
            desc[0] += static_cast<float>(rx * shoulder);
            desc[1] += static_cast<float>(ry * shoulder);
            ++g_shoulder_frames;
            changed = true;
        }
    }
    o_set_camera(self, desc);
    if (changed) std::memcpy(desc, saved, sizeof saved);
}
}  // namespace

void install() {
    if (!readable(reinterpret_cast<const void*>(kSetCameraSlot), 4) || *reinterpret_cast<const uint32_t*>(kSetCameraSlot) != kSetCamera) {
        std::snprintf(g_status, sizeof g_status, "camera follow every frame: engine code not where expected, left as is");
        return;
    }
    o_set_camera = reinterpret_cast<SetCamera_t>(kSetCamera);
    uint32_t* slot = reinterpret_cast<uint32_t*>(kSetCameraSlot);
    DWORD old = 0;
    if (!VirtualProtect(slot, 4, PAGE_READWRITE, &old)) return;
    *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_set_camera));
    VirtualProtect(slot, 4, old, &old);
    std::snprintf(g_status, sizeof g_status, "camera follow every frame: %s",
                  config().camera_latch ? "on (learning the mouse scale while you turn)" : "off in fable_online.ini (measuring only)");
}

void on_update(float yaw_turns, const float focus[3]) {
    // Yaw per count in Fable's own units, for the log only.
    g.win_turn += yaw_turns;
    g.win_counts += static_cast<double>(g.counts_since);
    if (std::fabs(g.win_counts) >= 300) {
        const double k = g.win_turn / g.win_counts;
        if (std::isfinite(k) && std::fabs(k) < 0.01) {
            g.turns_per_count = g.k_windows ? 0.7 * g.turns_per_count + 0.3 * k : k;
            ++g.k_windows;
        }
        g.win_turn = g.win_counts = 0;
    }
    g.update_at = now_s();
    g.last_turn = yaw_turns;
    g.consumed = g.counts_since;
    g.counts_since = 0;
    std::memcpy(g.focus, focus, sizeof g.focus);
    g.updated_since_frame = true;
}

void on_mouse(long dx) { g.counts_since += dx; }

const char* status() { return g_status; }

void log_stats() {
    const double fit_r2 = g.s_cc > 0 && g.s_jj > 0 ? g.s_jc * g.s_jc / (g.s_cc * g.s_jj) : 0;
    fo::log("camera: follow every frame %s; %.6f turns a count; view %.5f rad a count (fit %.2f, %u samples)%s; "
            "frames between updates %s; %u of %u frames led, by up to %.3f rad",
            config().camera_latch ? "on" : "off", g.turns_per_count, g.s_cc > 0 ? g.s_jc / g.s_cc : 0.0, fit_r2, g.fit_samples,
            rad_per_count() != 0 ? "" : " (not trusted yet)", g.moved_between > 2 * g.still_between ? "blend" : "hold", g.turned,
            g.frames, g.max_lead);
    fo::log("camera: smoothing %s: %u frames eased, %u cuts; height moved %.2f by Fable, %.2f on screen; "
            "shoulder offset on %u frames (%.2f)",
            config().camera_smooth ? "on" : "off", sm.frames, sm.snaps, sm.raw_dz, sm.shown_dz, g_shoulder_frames,
            static_cast<double>(g_swapped ? -config().camera_shoulder : config().camera_shoulder));
    sm.frames = sm.snaps = 0;
    sm.raw_dz = sm.shown_dz = 0;
    g_shoulder_frames = 0;
    g.frames = g.turned = 0;
    g.max_lead = 0;
}
}  // namespace fo::display::camera_latch
