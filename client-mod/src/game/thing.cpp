#include "thing.h"

#include <windows.h>

#include <cmath>
#include <cstring>

#include "../log.h"

namespace fo::game {
namespace {

uintptr_t g_hero = 0;
DWORD g_lastScan = 0;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool write(uintptr_t addr, const void* in, size_t n) {
    SIZE_T put = 0;
    return addr > 0x10000 &&
           WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(addr), in, n, &put) && put == n;
}

bool sane(float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; }

bool looks_like_hero(uintptr_t a) {
    uint32_t vt = 0, phys = 0, pvt = 0;
    return read(a, &vt, 4) && vt == kHeroVtable && read(a + kPhysicsPtr, &phys, 4) && phys &&
           read(phys, &pvt, 4) && pvt == kPhysicsVtable;
}

/** Walks the process's own private, writable, committed memory. */
template <typename Fn>
void each_region(Fn&& fn) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
        uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize < (256u << 20))
            fn(lo, mbi.RegionSize);
        if (mbi.RegionSize == 0) break;
        p = lo + mbi.RegionSize;
    }
}

}  // namespace

uintptr_t physics_of(uintptr_t thing) {
    uint32_t phys = 0, pvt = 0;
    if (!read(thing + kPhysicsPtr, &phys, 4) || !phys) return 0;
    if (!read(phys, &pvt, 4)) return 0;
    return phys;
}

uintptr_t local_hero() {
    if (g_hero && !looks_like_hero(g_hero)) {
        g_hero = 0;
        fo::log("thing: lost the hero (area change or front end)");
    }
    if (g_hero) return g_hero;
    if (GetTickCount() - g_lastScan < 3000) return 0;  // a scan is slow; not on every tick
    g_lastScan = GetTickCount();
    std::vector<uint32_t> buf;
    uintptr_t found = 0;
    each_region([&](uintptr_t lo, size_t size) {
        if (found) return;
        buf.resize(size / 4);
        if (!read(lo, buf.data(), buf.size() * 4)) return;
        for (size_t i = 0; i < buf.size(); ++i)
            if (buf[i] == kHeroVtable && looks_like_hero(lo + i * 4)) {
                found = lo + i * 4;
                return;
            }
    });
    if (found) {
        g_hero = found;
        fo::log("thing: hero at 0x%08X", static_cast<unsigned>(found));
    }
    return g_hero;
}

void forget_hero() { g_hero = 0; }

bool read_pos(uintptr_t thing, Pos& out) {
    uintptr_t phys = physics_of(thing);
    if (!phys) return false;
    float p[3];
    if (!read(phys + kPosOffset, p, sizeof(p))) return false;
    if (!sane(p[0]) || !sane(p[1]) || !sane(p[2])) return false;
    out = Pos{p[0], p[1], p[2]};
    return true;
}

bool read_yaw(uintptr_t thing, float& yaw) {
    uintptr_t phys = physics_of(thing);
    if (!phys) return false;
    float f[2];
    if (!read(phys + kForwardOffset, f, sizeof(f))) return false;
    if (!sane(f[0]) || !sane(f[1]) || (f[0] == 0 && f[1] == 0)) return false;
    yaw = std::atan2(-f[0], f[1]);
    return true;
}

bool write_pos(uintptr_t thing, const Pos& p, bool have_yaw, float yaw) {
    if (!sane(p.x) || !sane(p.y) || !sane(p.z)) return false;
    uintptr_t phys = physics_of(thing);
    if (!phys) return false;
    const float xyz[3] = {p.x, p.y, p.z};
    // Both the live position and the one from the last physics step, or the engine's own
    // step would drag the thing back towards where it thought it was.
    bool ok = write(phys + kPosOffset, xyz, sizeof(xyz)) && write(phys + kPrevPosOffset, xyz, sizeof(xyz));
    if (ok && have_yaw && std::isfinite(yaw)) {
        const float fwd[2] = {-std::sin(yaw), std::cos(yaw)};
        write(phys + kForwardOffset, fwd, sizeof(fwd));
        write(phys + kPrevForwardOffset, fwd, sizeof(fwd));
    }
    return ok;
}

uint32_t area_id(uintptr_t thing) {
    float a[3];
    if (!read(thing + kAreaAnchor, a, sizeof(a))) return 0;
    uint32_t h = 2166136261u;
    for (float v : a) {
        if (!sane(v)) return 0;
        int32_t r = static_cast<int32_t>(std::lround(v));
        for (int b = 0; b < 4; ++b) {
            h ^= static_cast<uint32_t>((r >> (b * 8)) & 0xFF);
            h *= 16777619u;
        }
    }
    return h & 0x7FFFFFFF;
}

std::vector<uintptr_t> scan_vtable(uint32_t vtable, size_t limit) {
    std::vector<uintptr_t> out;
    std::vector<uint32_t> buf;
    each_region([&](uintptr_t lo, size_t size) {
        if (out.size() >= limit) return;
        buf.resize(size / 4);
        if (!read(lo, buf.data(), buf.size() * 4)) return;
        for (size_t i = 0; i < buf.size() && out.size() < limit; ++i)
            if (buf[i] == vtable) out.push_back(lo + i * 4);
    });
    return out;
}

}  // namespace fo::game
