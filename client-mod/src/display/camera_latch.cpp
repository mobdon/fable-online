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
    const double r = config().camera_latch && mouse_look ? lead(now) : 0.0;
    const float shoulder = mouse_look ? config().camera_shoulder : 0.0f;
    if (std::fabs(r) < 1e-5 && shoulder == 0.0f) { o_set_camera(self, desc); return; }
    float saved[9];
    std::memcpy(saved, desc, sizeof saved);
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
        }
    }
    o_set_camera(self, desc);
    std::memcpy(desc, saved, sizeof saved);
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
    g.frames = g.turned = 0;
    g.max_lead = 0;
}
}  // namespace fo::display::camera_latch
