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
    float last_turn = 0;          // yaw turn of the last update (turns)
    float focus[3] = {};
    long counts_since = 0;        // mouse counts since that update
    // Learning: yaw turns per mouse count, over windows with enough motion.
    double win_turn = 0, win_counts = 0;
    double turns_per_count = 0;
    int k_windows = 0;
    // Which way the rendered view turns for a positive Fable yaw, from the view direction seen
    // at SetCamera across updates.
    double render_turn_since = 0;  // rendered heading change since the last update (radians)
    double sign_votes = 0;
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

// The view's lead over what Fable shows, in radians of heading (0 while still learning).
double lead(double now) {
    if (g.k_windows < 3 || std::fabs(g.sign_votes) < 0.02) return 0;
    const double since = now - g.update_at;
    if (since > 0.25) return 0;  // mouse-look isn't running (menus, cut scenes)
    double frac = since / kTickSeconds;
    if (frac > 1) frac = 1;
    const bool blends = g.moved_between > 2 * g.still_between;
    double turns = g.turns_per_count * static_cast<double>(g.counts_since);
    if (blends) turns += (1.0 - frac) * g.last_turn;
    const double sign = g.sign_votes > 0 ? 1.0 : -1.0;
    double r = sign * kTwoPi * turns;
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
        g.render_turn_since += d;
        if (!g.updated_since_frame) {
            if (std::fabs(d) > 1e-6) ++g.moved_between; else ++g.still_between;
        }
    }
    g.have_heading = true;
    g.last_heading = h;
    g.updated_since_frame = false;
    ++g.frames;

    const double dx = desc[0] - g.focus[0], dy = desc[1] - g.focus[1], dz = desc[2] - g.focus[2];
    const bool near_hero = dx * dx + dy * dy + dz * dz < 40.0 * 40.0;  // the mouse-look camera, not a cut scene's
    const double r = config().camera_latch && near_hero ? lead(now) : 0.0;
    if (std::fabs(r) < 1e-5) { o_set_camera(self, desc); return; }
    float saved[9];
    std::memcpy(saved, desc, sizeof saved);
    const double c = std::cos(r), s = std::sin(r);
    float off[3] = {desc[0] - g.focus[0], desc[1] - g.focus[1], desc[2] - g.focus[2]};
    rotate_z(off, c, s);
    desc[0] = g.focus[0] + off[0];
    desc[1] = g.focus[1] + off[1];
    rotate_z(desc + 3, c, s);
    rotate_z(desc + 6, c, s);
    ++g.turned;
    if (std::fabs(r) > g.max_lead) g.max_lead = std::fabs(r);
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
    // Learn which way the view turns: the rendered heading change over the last update
    // interval against Fable's yaw turn that drove it.
    if (std::fabs(yaw_turns) > 1e-4 && std::fabs(g.render_turn_since) > 1e-4) {
        g.sign_votes += (yaw_turns > 0) == (g.render_turn_since > 0) ? 0.01 : -0.01;
        if (g.sign_votes > 1) g.sign_votes = 1;
        if (g.sign_votes < -1) g.sign_votes = -1;
    }
    g.render_turn_since = 0;
    // Learn yaw per count over windows of at least 300 counts of motion.
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
    g.counts_since = 0;
    std::memcpy(g.focus, focus, sizeof g.focus);
    g.updated_since_frame = true;
}

void on_mouse(long dx) { g.counts_since += dx; }

const char* status() { return g_status; }

void log_stats() {
    fo::log("camera: follow every frame %s; %.6f turns a count (%d samples), view turns %s, frames between updates %s; "
            "%u of %u frames led, by up to %.3f rad",
            config().camera_latch ? "on" : "off", g.turns_per_count, g.k_windows,
            std::fabs(g.sign_votes) < 0.02 ? "unknown yet" : g.sign_votes > 0 ? "with the yaw" : "against the yaw",
            g.moved_between > 2 * g.still_between ? "blend" : "hold", g.turned, g.frames, g.max_lead);
    g.frames = g.turned = 0;
    g.max_lead = 0;
}
}  // namespace fo::display::camera_latch
