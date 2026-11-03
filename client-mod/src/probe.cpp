#include "probe.h"
#include "log.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// vtable addresses from tools/re-notes/rtti.json (Fable.exe has no ASLR, so these are fixed).
namespace {
struct Target {
    const char* name;
    uint32_t vtable;
    uint32_t bytes;            // how much of the object to watch
    std::vector<uintptr_t> found;
    std::vector<std::vector<uint8_t>> prev;
};

Target g_targets[] = {
    {"CThingPlayerCreature", 0x012457FC, 0x1000, {}, {}},
    {"CTCHeroStats",         0x0124F70C, 0x200, {}, {}},
    {"CTCHeroExperience",    0, 0x200, {}, {}},  // filled at start if known
};

bool safe_read(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

// Walk committed, writable, private memory and collect objects whose first dword is the vtable.
void scan() {
    for (auto& t : g_targets) { t.found.clear(); t.prev.clear(); }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t p = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    uintptr_t end = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    std::vector<uint8_t> buf;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<LPCVOID>(p), &mbi, sizeof(mbi))) break;
        uintptr_t lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        uintptr_t size = mbi.RegionSize;
        bool rw = (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) && !(mbi.Protect & PAGE_GUARD);
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && rw && size < (256u << 20)) {
            buf.resize(size);
            if (safe_read(lo, buf.data(), size)) {
                for (size_t off = 0; off + 4 <= size; off += 4) {
                    uint32_t v;
                    std::memcpy(&v, &buf[off], 4);
                    for (auto& t : g_targets)
                        if (t.vtable && v == t.vtable && t.found.size() < 2) t.found.push_back(lo + off);
                }
            }
        }
        p = lo + size;
    }
    for (auto& t : g_targets) {
        if (!t.vtable) continue;
        fo::log("probe: %s x%u", t.name, static_cast<unsigned>(t.found.size()));
        for (auto a : t.found) {
            fo::log("  at 0x%08X", static_cast<unsigned>(a));
            // One-off dump: every dword, shown as hex and float, so we can spot
            // world coordinates (big floats) and component pointers (heap addresses).
            std::vector<uint8_t> raw(t.bytes);
            if (!safe_read(a, raw.data(), t.bytes)) continue;
            for (uint32_t off = 0; off + 4 <= t.bytes; off += 4) {
                uint32_t u; float f;
                std::memcpy(&u, &raw[off], 4);
                std::memcpy(&f, &raw[off], 4);
                if (u == 0) continue;
                // A heap pointer to an object with a vtable in .rdata = a component: log its vtable.
                uint32_t vt = 0;
                if (u >= 0x00100000 && u < 0x7FFF0000 && (u & 3) == 0 && safe_read(u, &vt, 4) &&
                    vt >= 0x01200000 && vt < 0x01400000) {
                    fo::log("  dump %s+0x%03X  %08X  -> obj vtable %08X", t.name, off, u, vt);
                } else {
                    fo::log("  dump %s+0x%03X  %08X  %g  (int %d)", t.name, off, u, std::isfinite(f) ? f : 0.0f, static_cast<int>(u));
                }
            }
        }
        t.prev.resize(t.found.size());
    }
}

// Log 4-byte fields that changed and look like sane floats (positions, health, timers).
void diff(Target& t) {
    for (size_t i = 0; i < t.found.size(); ++i) {
        std::vector<uint8_t> now(t.bytes);
        if (!safe_read(t.found[i], now.data(), t.bytes)) continue;
        uint32_t vt;
        std::memcpy(&vt, now.data(), 4);
        if (vt != t.vtable) { t.found[i] = 0; continue; }  // object freed; rescan later
        auto& before = t.prev[i];
        if (before.size() == now.size()) {
            int lines = 0;
            for (uint32_t off = 4; off + 4 <= t.bytes && lines < 24; off += 4) {
                if (std::memcmp(&before[off], &now[off], 4) == 0) continue;
                float a, b;
                std::memcpy(&a, &before[off], 4);
                std::memcpy(&b, &now[off], 4);
                if (std::isfinite(a) && std::isfinite(b) && std::fabs(b) < 1e6f && std::fabs(b) > 1e-4f) {
                    fo::log("  %s[%u]+0x%03X  %.3f -> %.3f", t.name, static_cast<unsigned>(i), off, a, b);
                    ++lines;
                } else {
                    int32_t ia, ib;
                    std::memcpy(&ia, &before[off], 4);
                    std::memcpy(&ib, &now[off], 4);
                    if (ib > -1000000 && ib < 1000000) {  // health, gold and counters are often ints
                        fo::log("  %s[%u]+0x%03X  int %d -> %d", t.name, static_cast<unsigned>(i), off, ia, ib);
                        ++lines;
                    }
                }
            }
        }
        before = std::move(now);
    }
}

// The hero's physics component (pointer at hero+0x60, CTCPhysicsControlled) is where the
// live position should be. Watch it the same way as the hero itself.
Target g_physics = {"CTCPhysicsControlled", 0, 0x300, {}, {}};

void watch_physics() {
    auto& hero = g_targets[0];
    if (hero.found.empty() || !hero.found[0]) return;
    uint32_t ptr = 0;
    if (!safe_read(hero.found[0] + 0x60, &ptr, 4) || !ptr) return;
    uint32_t vt = 0;
    if (!safe_read(ptr, &vt, 4)) return;
    if (g_physics.found.empty() || g_physics.found[0] != ptr) {
        g_physics.vtable = vt;
        g_physics.found.assign(1, ptr);
        g_physics.prev.assign(1, {});
        fo::log("probe: CTCPhysicsControlled at 0x%08X (vtable %08X)", ptr, vt);
        std::vector<uint8_t> raw(g_physics.bytes);
        if (safe_read(ptr, raw.data(), raw.size()))
            for (uint32_t off = 4; off + 4 <= g_physics.bytes; off += 4) {
                float f;
                std::memcpy(&f, &raw[off], 4);
                if (std::isfinite(f) && std::fabs(f) > 1.0f && std::fabs(f) < 100000.0f)
                    fo::log("  dump CTCPhysicsControlled+0x%03X  %g", off, f);
            }
    }
    diff(g_physics);
}
}  // namespace

namespace fo {
void probe_tick() {
    static unsigned tick = 0;
    bool stale = false;
    for (auto& t : g_targets)
        if (t.vtable && (t.found.empty() || !t.found[0])) stale = true;
    if (tick % 15 == 0 || stale) scan();  // full scan every ~30 s, or when an object vanished
    for (auto& t : g_targets) if (t.vtable) diff(t);
    watch_physics();
    ++tick;
}
}  // namespace fo
