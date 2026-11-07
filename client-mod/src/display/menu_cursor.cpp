#include "menu_cursor.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "../log.h"
#include "mouse_look.h"

namespace fo::display::menu_cursor {
namespace {
struct Fit {
    double sxy = 0, sxx = 0, syy = 0;
    unsigned n = 0;
    void add(double p, double d) {
        sxy = 0.97 * sxy + p * d;
        sxx = 0.97 * sxx + p * p;
        syy = 0.97 * syy + d * d;
        ++n;
    }
    double ratio() const { return sxx > 0 ? sxy / sxx : 0; }
    double r2() const { return sxx > 0 && syy > 0 ? (sxy * sxy) / (sxx * syy) : 0; }
    bool trusted() const { return n >= 12 && r2() >= 0.9 && std::fabs(ratio()) > 1e-6; }
} g_fx, g_fy;

// Which texture is the cursor.
void* g_frame_tex = nullptr;      // texture of this frame's last single-quad sprite
float g_frame_pos[2] = {};        // its first vertex (clip space)
void* g_candidate = nullptr;
unsigned g_streak = 0;
void* g_cursor_tex = nullptr;

// Motion.
long g_pend_x = 0, g_pend_y = 0;          // polled since Fable's last read
long g_taken_x = 0, g_taken_y = 0;        // taken in by reads since the quad last moved
bool g_have_last = false;
float g_last_pos[2] = {};
unsigned g_polls = 0, g_reads = 0, g_moved = 0, g_cursor_frames = 0;
DWORD g_logged_at = 0;

void log_now() {
    fo::log("menu cursor: %s; %u mouse polls, %u reads; fit x %.6f (r2 %.2f, %u), y %.6f (r2 %.2f, %u); %u of %u cursor draws moved ahead",
            g_cursor_tex ? "cursor picture found" : "looking for the cursor picture", g_polls, g_reads, g_fx.ratio(), g_fx.r2(), g_fx.n,
            g_fy.ratio(), g_fy.r2(), g_fy.n, g_moved, g_cursor_frames);
    g_polls = g_reads = g_moved = g_cursor_frames = 0;
}
}  // namespace

const void* sprite(IDirect3DDevice9* d, const void* data, UINT first, UINT count, UINT prims, UINT stride) {
    if (prims != 2 || count != 4 || !data || stride < 12) return nullptr;
    IDirect3DBaseTexture9* tex = nullptr;
    if (FAILED(d->GetTexture(0, &tex)) || !tex) return nullptr;
    tex->Release();   // only the pointer is kept, as an identity
    const float* v = reinterpret_cast<const float*>(static_cast<const uint8_t*>(data) + static_cast<size_t>(first) * stride);
    g_frame_tex = tex;
    g_frame_pos[0] = v[0];
    g_frame_pos[1] = v[1];
    if (tex != g_cursor_tex || mouse_look::in_game()) return nullptr;
    ++g_cursor_frames;
    if (!g_fx.trusted() || !g_fy.trusted()) return nullptr;
    // Ahead by what Fable hasn't shown yet: motion polled since its last read, plus motion it
    // has read but not yet moved the quad for.
    double ox = (g_pend_x + g_taken_x) * g_fx.ratio(), oy = (g_pend_y + g_taken_y) * g_fy.ratio();
    if (ox > 0.5) ox = 0.5; else if (ox < -0.5) ox = -0.5;
    if (oy > 0.5) oy = 0.5; else if (oy < -0.5) oy = -0.5;
    if (ox == 0 && oy == 0) return nullptr;
    static std::vector<uint8_t> buf;
    const size_t bytes = static_cast<size_t>(first + count) * stride;
    if (buf.size() < bytes) buf.resize(bytes);
    std::memcpy(buf.data() + static_cast<size_t>(first) * stride, static_cast<const uint8_t*>(data) + static_cast<size_t>(first) * stride,
                static_cast<size_t>(count) * stride);
    for (UINT i = 0; i < count; ++i) {
        float* p = reinterpret_cast<float*>(buf.data() + static_cast<size_t>(first + i) * stride);
        p[0] = static_cast<float>(p[0] + ox);
        p[1] = static_cast<float>(p[1] + oy);
    }
    ++g_moved;
    return buf.data();
}

void on_motion(long dx, long dy) {
    ++g_polls;
    g_pend_x += dx;
    g_pend_y += dy;
}

void before_read() {
    ++g_reads;
    g_taken_x += g_pend_x;
    g_taken_y += g_pend_y;
    g_pend_x = g_pend_y = 0;
    const DWORD now = GetTickCount();
    if (now - g_logged_at > 30000) {
        g_logged_at = now;
        log_now();
    }
}

void on_present() {
    void* tex = g_frame_tex;
    g_frame_tex = nullptr;
    if (mouse_look::in_game() || !tex) {
        g_have_last = false;
        return;
    }
    if (!g_cursor_tex) {
        if (tex == g_candidate) {
            if (++g_streak >= 120) {
                g_cursor_tex = tex;
                fo::log("menu cursor: found the cursor picture (texture %p)", tex);
            }
        } else {
            g_candidate = tex;
            g_streak = 1;
        }
        return;
    }
    if (tex != g_cursor_tex) return;
    // Learn: when the quad moves, compare with the motion Fable took in since it last moved.
    if (g_have_last && (g_frame_pos[0] != g_last_pos[0] || g_frame_pos[1] != g_last_pos[1])) {
        if (g_taken_x != 0) g_fx.add(g_taken_x, g_frame_pos[0] - g_last_pos[0]);
        if (g_taken_y != 0) g_fy.add(g_taken_y, g_frame_pos[1] - g_last_pos[1]);
        g_taken_x = g_taken_y = 0;
    }
    g_last_pos[0] = g_frame_pos[0];
    g_last_pos[1] = g_frame_pos[1];
    g_have_last = true;
}
}  // namespace fo::display::menu_cursor
