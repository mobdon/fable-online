#include "kills.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_set>

#include "../log.h"
#include "spawns.h"
#include "thing.h"

namespace fo::game::kills {
namespace {

// __thiscall (thing, float delta, int flag), pops 8 (tools/re-notes/11-hero-health.md).
constexpr uintptr_t kModifyHealth = 0x006A62E0;
constexpr uintptr_t kCalls[] = {0x006A6858, 0x006A68A5, 0x006A6984};
using ModifyFn = void(__fastcall*)(uintptr_t thing, uintptr_t unused, float delta, int flag);

std::atomic<bool> g_on{false};
bool g_installed = false;
std::mutex g_mu;
std::vector<Kill> g_kills;
float g_hurt = 0;
std::unordered_set<uintptr_t> g_reported;
unsigned g_logged = 0;

bool read_mem(uintptr_t addr, void* out, size_t n) {
    SIZE_T got = 0;
    return addr > 0x10000 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), out, n, &got) && got == n;
}

bool calls(uintptr_t site, uintptr_t target) {
    unsigned char b[5];
    if (!read_mem(site, b, 5) || b[0] != 0xE8) return false;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return site + 5 + static_cast<uintptr_t>(rel) == target;
}

bool retarget(uintptr_t site, uintptr_t to) {
    int32_t rel = static_cast<int32_t>(to - (site + 5));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(site + 1), 4, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(site + 1), &rel, 4);
    VirtualProtect(reinterpret_cast<LPVOID>(site + 1), 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<LPCVOID>(site), 5);
    return true;
}

void __fastcall modify_health_hook(uintptr_t thing, uintptr_t edx, float delta, int flag) {
    const auto orig = reinterpret_cast<ModifyFn>(kModifyHealth);
    if (!g_on.load(std::memory_order_relaxed)) return orig(thing, edx, delta, flag);
    float hp = 0, max = 0;
    const bool known = read_health(thing, hp, max);
    const uintptr_t hero = cached_hero();
    if (known && thing == hero && hero) {
        // Fable's own death is a reload: keep the hero standing; the server decides the fall.
        if (delta < 0 && hp + delta < 1.0f) delta = hp > 1.0f ? 1.0f - hp : 0.0f;
        orig(thing, edx, delta, flag);
        float after = 0, max2 = 0;
        if (read_health(thing, after, max2) && after < hp && max > 0) {
            std::lock_guard<std::mutex> lock(g_mu);
            g_hurt += (hp - after) / max;
        }
        return;
    }
    orig(thing, edx, delta, flag);
    if (!known || hp <= 0 || not_local_hero(thing)) return;
    float after = 0, max2 = 0;
    // A thing being removed reads as unknown: that is a death too.
    const bool now = read_health(thing, after, max2);
    if (now && after > 0) return;
    std::string def = spawns::def_name(thing);
    const std::string level = spawns::level_def_of(thing);
    if (def.empty()) def = level;
    Pos at;
    read_pos(thing, at);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_reported.insert(thing).second) return;
    if (g_reported.size() > 2048) g_reported.clear();
    if (g_logged < 40) {
        ++g_logged;
        fo::log("kills: %s died in this game at %.1f %.1f %.1f (engine def %s, level def %s)", def.empty() ? "?" : def.c_str(),
                at.x, at.y, at.z, def.empty() ? "-" : def.c_str(), level.empty() ? "-" : level.c_str());
    }
    if (def.empty()) return;
    g_kills.push_back(Kill{def, at.x, at.y, at.z});
}

}  // namespace

bool install() {
    if (g_installed) return true;
    for (uintptr_t site : kCalls)
        if (!calls(site, kModifyHealth)) {
            fo::log("kills: health call at 0x%08X is not what this build expects; kills are not reported",
                    static_cast<unsigned>(site));
            return false;
        }
    for (uintptr_t site : kCalls) retarget(site, reinterpret_cast<uintptr_t>(&modify_health_hook));
    g_installed = true;
    fo::log("kills: health calls hooked (%u)", static_cast<unsigned>(std::size(kCalls)));
    return true;
}

void set_enabled(bool on) {
    if (on != g_on.load()) fo::log("kills: client monsters %s", on ? "on (this game runs its monsters)" : "off");
    g_on = on && g_installed;
    std::lock_guard<std::mutex> lock(g_mu);
    g_kills.clear();
    g_hurt = 0;
    g_reported.clear();
}

bool enabled() { return g_on.load(); }

std::vector<Kill> drain_kills() {
    std::lock_guard<std::mutex> lock(g_mu);
    std::vector<Kill> out;
    out.swap(g_kills);
    return out;
}

float drain_hurt() {
    std::lock_guard<std::mutex> lock(g_mu);
    float h = g_hurt;
    g_hurt = 0;
    return h;
}

}  // namespace fo::game::kills
