#include "world_time.h"

#include <windows.h>

#include <cmath>

namespace fo::game::world_time {
namespace {

// Fable.exe has no ASLR, so these are fixed. See tools/re-notes/08-world-time.md.
constexpr uintptr_t kGameGlobal = 0x013B8A1C;   // [this]+0x24 = the CWorld (getter 0x00686A70)
constexpr uint32_t kWorldPtr = 0x24;
constexpr uint32_t kWorldVtable = 0x012390F0;   // CWorld's primary vtable (0x012390DC is at +4)
constexpr uint32_t kTimeManager = 0x1C;         // world -> CGameTimeManager
constexpr uint32_t kOwner = 0x00;               // manager -> its CWorld
constexpr uint32_t kTime = 0x08;                // float, fraction of a day
constexpr uint32_t kTimePrev = 0x0C;            // last update's time
constexpr uint32_t kSpeed = 0x10;               // float, days per game second
constexpr uint32_t kDayCount = 0x14;
constexpr uint32_t kTransition = 0x21;          // byte: easing to +0x24 over +0x28 seconds
constexpr uint32_t kTimeCopy = 0x1C;            // float, follows +0x08 outside day-only mode
constexpr uint32_t kPause = 0x30;               // non-zero: the clock does not run

bool rd(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
}

bool wr(uintptr_t a, const void* in, size_t n) {
    SIZE_T put = 0;
    return a && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), in, n, &put) && put == n;
}

bool sane(float f) { return std::isfinite(f) && f >= 0.0f && f <= 1.0f; }

}  // namespace

bool read(Clock& out) {
    uint32_t game = 0, world = 0, wvt = 0, mgr = 0, owner = 0;
    if (!rd(kGameGlobal, &game, 4) || !rd(game + kWorldPtr, &world, 4)) return false;
    if (!rd(world, &wvt, 4) || wvt != kWorldVtable) return false;
    if (!rd(world + kTimeManager, &mgr, 4) || !rd(mgr + kOwner, &owner, 4) || owner != world) return false;
    Clock c;
    c.manager = mgr;
    uint8_t transition = 0;
    uint32_t pause = 0;
    if (!rd(mgr + kTime, &c.time, 4) || !rd(mgr + kSpeed, &c.speed, 4) || !rd(mgr + kDayCount, &c.day, 4) ||
        !rd(mgr + kTransition, &transition, 1) || !rd(mgr + kPause, &pause, 4))
        return false;
    if (!sane(c.time) || !std::isfinite(c.speed)) return false;
    c.transition = transition != 0;
    c.paused = pause != 0;
    out = c;
    return true;
}

bool write_time(const Clock& c, float fraction) {
    if (!c.manager || !std::isfinite(fraction)) return false;
    fraction -= std::floor(fraction);
    // +0x0C is last update's time: setting it too keeps Fable from treating the jump as time
    // that passed (the same as its own SetTimeOfDay console command does).
    return wr(c.manager + kTime, &fraction, 4) && wr(c.manager + kTimePrev, &fraction, 4) &&
           wr(c.manager + kTimeCopy, &fraction, 4);
}

bool write_speed(const Clock& c, float speed) {
    if (!c.manager || !std::isfinite(speed) || speed < 0.0f || speed > 0.01f) return false;
    return wr(c.manager + kSpeed, &speed, 4);
}

}  // namespace fo::game::world_time
