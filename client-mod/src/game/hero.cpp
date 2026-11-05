#include "hero.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include "../log.h"

namespace fo::game {
bool not_local_hero(uintptr_t thing);   // thing.cpp (thing.h would clash with the offsets below)

namespace {
constexpr uint32_t kHeroVtable = 0x012457FC;      // CThingPlayerCreature
constexpr uint32_t kPhysicsVtable = 0x0126616C;   // CTCPhysicsControlled
constexpr uint32_t kPhysicsPtr = 0x060;           // hero -> physics component
constexpr uint32_t kPos = 0x00C;                  // physics -> x, y, z floats
constexpr uint32_t kAreaAnchor = 0x0E8;           // hero -> area anchor x, y, z (changes on area change)

uintptr_t g_hero = 0;
DWORD g_last_scan = 0;

bool read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool looks_like_hero(uintptr_t a) {
    uint32_t vt = 0, phys = 0, pvt = 0;
    return read(a, &vt, 4) && vt == kHeroVtable && read(a + kPhysicsPtr, &phys, 4) && phys &&
           read(phys, &pvt, 4) && pvt == kPhysicsVtable;
}

uintptr_t scan() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    // Read in fixed 64 KB pieces: one buffer the size of a large region (up to 256 MB) could
    // not be allocated in Fable's crowded 32-bit address space, and the failed allocation
    // ended the whole game.
    static uint32_t buf[16384];
    constexpr size_t kChunk = sizeof(buf);
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
        uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && mbi.RegionSize < (256u << 20)) {
            for (size_t off = 0; off < mbi.RegionSize; off += kChunk) {
                size_t n = mbi.RegionSize - off < kChunk ? mbi.RegionSize - off : kChunk;
                if (!read(lo + off, buf, n)) break;
                for (size_t i = 0; i < n / 4; ++i)
                    if (buf[i] == kHeroVtable && looks_like_hero(lo + off + i * 4) &&
                        !not_local_hero(lo + off + i * 4))  // stand-ins use the hero's def
                        return lo + off + i * 4;
            }
        }
        p = lo + mbi.RegionSize;
    }
    return 0;
}

uint32_t hash_area(const float a[3]) {
    // Round so float noise doesn't change the id; FNV-1a over the rounded ints.
    uint32_t h = 2166136261u;
    for (int i = 0; i < 3; ++i) {
        int32_t v = static_cast<int32_t>(std::lround(a[i]));
        for (int b = 0; b < 4; ++b) { h ^= (v >> (b * 8)) & 0xFF; h *= 16777619u; }
    }
    return h & 0x7FFFFFFF;
}
}  // namespace

HeroPos hero_peek() {
    HeroPos out;
    const uintptr_t h = g_hero;
    uint32_t phys = 0;
    float pos[3];
    if (!h || !read(h + kPhysicsPtr, &phys, 4) || !read(phys + kPos, pos, sizeof(pos))) return out;
    for (float v : pos) if (!std::isfinite(v) || std::fabs(v) > 1e6f) return out;
    out.valid = true;
    out.x = pos[0]; out.y = pos[1]; out.z = pos[2];
    return out;
}

HeroPos hero_read() {
    HeroPos out;
    if (g_hero && (!looks_like_hero(g_hero) || not_local_hero(g_hero))) { g_hero = 0; fo::log("hero: lost (area change or menu)"); }
    if (!g_hero) {
        if (GetTickCount() - g_last_scan < 3000) return out;  // scanning is slow; not every tick
        g_last_scan = GetTickCount();
        g_hero = scan();
        if (!g_hero) return out;
        fo::log("hero: found at 0x%08X", static_cast<unsigned>(g_hero));
    }
    uint32_t phys = 0;
    float pos[3], anchor[3];
    if (!read(g_hero + kPhysicsPtr, &phys, 4) || !read(phys + kPos, pos, sizeof(pos)) ||
        !read(g_hero + kAreaAnchor, anchor, sizeof(anchor)))
        return out;
    for (float v : pos) if (!std::isfinite(v) || std::fabs(v) > 1e6f) return out;
    out.valid = true;
    out.x = pos[0]; out.y = pos[1]; out.z = pos[2];
    out.area = hash_area(anchor);
    return out;
}
}  // namespace fo::game
