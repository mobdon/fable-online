#include "menu_cursor.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include "../log.h"
#include "mouse_look.h"

namespace fo::display::menu_cursor {
namespace {
constexpr uint32_t kVtable = 0x0124C22C, kSlot = kVtable + 23 * 4, kOriginal = 0x0055C600;
using Pos_t = float*(__fastcall*)(void*, void*, float*);
Pos_t o_pos = nullptr;

struct Fit {
    double sxy = 0, sxx = 0, syy = 0;
    unsigned n = 0;
    void add(double p, double d) {
        sxy = 0.98 * sxy + p * d;
        sxx = 0.98 * sxx + p * p;
        syy = 0.98 * syy + d * d;
        ++n;
    }
    double ratio() const { return sxx > 0 ? sxy / sxx : 0; }
    double r2() const { return sxx > 0 && syy > 0 ? (sxy * sxy) / (sxx * syy) : 0; }
    bool trusted() const { return n >= 12 && r2() >= 0.9 && ratio() > 0.01 && ratio() < 20; }
} g_fx, g_fy;

bool g_have_rest = false;
POINT g_rest = {};              // where the cursor was right after Fable's last read
long g_read_dx = 0, g_read_dy = 0;   // movement Fable's last read took in
float g_pos_at_read[2] = {};
float g_last_pos[2] = {};
bool g_have_pos = false, g_waiting = false;
unsigned g_calls = 0, g_reads = 0, g_shifted = 0;
DWORD g_logged_at = 0;

POINT cursor() {
    POINT p = {};
    GetCursorPos(&p);
    return p;
}

void log_now() {
    fo::log("menu cursor: %u draws, %u mouse reads; fit x %.3f (r2 %.2f), y %.3f (r2 %.2f), %u/%u samples; %s; %u draws moved ahead",
            g_calls, g_reads, g_fx.ratio(), g_fx.r2(), g_fy.ratio(), g_fy.r2(), g_fx.n, g_fy.n,
            g_fx.trusted() && g_fy.trusted() ? "drawn at the mouse" : "learning (Fable's own steps)", g_shifted);
    g_calls = g_reads = g_shifted = 0;
}

float* __fastcall hk_pos(void* self, void* edx, float* out) {
    float* r = o_pos(self, edx, out);
    ++g_calls;
    const float x = out[0], y = out[1];
    if (g_waiting && g_have_pos && (x != g_pos_at_read[0] || y != g_pos_at_read[1])) {
        // Fable applied its last read: learn how far it moves the cursor per unit of movement.
        if (g_read_dx != 0 && x != g_pos_at_read[0]) g_fx.add(g_read_dx, x - g_pos_at_read[0]);
        if (g_read_dy != 0 && y != g_pos_at_read[1]) g_fy.add(g_read_dy, y - g_pos_at_read[1]);
        g_waiting = false;
    }
    g_last_pos[0] = x;
    g_last_pos[1] = y;
    g_have_pos = true;
    if (g_have_rest && !mouse_look::in_game() && g_fx.trusted() && g_fy.trusted()) {
        const POINT c = cursor();
        double px = c.x - g_rest.x, py = c.y - g_rest.y;
        if (g_waiting) { px += g_read_dx; py += g_read_dy; }   // read taken but not shown yet
        double ox = px * g_fx.ratio(), oy = py * g_fy.ratio();
        if (ox > 300) ox = 300; else if (ox < -300) ox = -300;
        if (oy > 300) oy = 300; else if (oy < -300) oy = -300;
        if (ox != 0 || oy != 0) {
            out[0] = static_cast<float>(x + ox);
            out[1] = static_cast<float>(y + oy);
            ++g_shifted;
        }
    }
    return r;
}
}  // namespace

void before_read() {
    ++g_reads;
    if (g_have_rest) {
        const POINT c = cursor();
        g_read_dx = c.x - g_rest.x;
        g_read_dy = c.y - g_rest.y;
        g_pos_at_read[0] = g_last_pos[0];
        g_pos_at_read[1] = g_last_pos[1];
        g_waiting = g_have_pos && (g_read_dx != 0 || g_read_dy != 0);
    }
    const DWORD now = GetTickCount();
    if (o_pos && now - g_logged_at > 30000) {
        g_logged_at = now;
        log_now();
    }
}

void after_read() {
    g_rest = cursor();
    g_have_rest = true;
}

void install() {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<void*>(kSlot), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return;
    if (*reinterpret_cast<const uint32_t*>(kSlot) != kOriginal) {
        fo::log("menu cursor: vtable not as expected, left as is");
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(kSlot), 4, PAGE_READWRITE, &old)) return;
    o_pos = reinterpret_cast<Pos_t>(kOriginal);
    *reinterpret_cast<uint32_t*>(kSlot) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_pos));
    VirtualProtect(reinterpret_cast<void*>(kSlot), 4, old, &old);
}
}  // namespace fo::display::menu_cursor
