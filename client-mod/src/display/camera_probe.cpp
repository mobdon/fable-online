#include "camera_probe.h"
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../log.h"

namespace fo::display::camera_probe {
namespace {
// 0x00B30B50: thiscall (params, bool, bool), ret 0Ch. Its first 8 bytes are
// sub esp,8 / push ebx / mov ebx,[esp+10h], which can run from anywhere.
constexpr uint32_t kFunc = 0x00B30B50;
constexpr uint8_t kStart[8] = {0x83, 0xEC, 0x08, 0x53, 0x8B, 0x5C, 0x24, 0x10};

using Set_t = void(__fastcall*)(void*, void*, const uint8_t*, uint32_t, uint32_t);
Set_t o_set = nullptr;

struct Caller {
    uint32_t ret = 0;
    unsigned count = 0;
    bool logged = false;
};
Caller g_callers[12];
unsigned g_lines = 0;

bool readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return reinterpret_cast<uintptr_t>(p) + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void log_first(uint32_t ret, const uint8_t* params) {
    char buf[700];
    int n = std::snprintf(buf, sizeof buf, "camera: engine camera set from %08X", ret);
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(__builtin_frame_address(0));
    for (int i = 0, found = 0; i < 200 && found < 8 && n < 300; ++i) {
        if (!readable(sp + i, 4)) break;
        const uint32_t v = sp[i];
        if (v > 0x00401000 && v < 0x00E00000 && v != ret) { n += std::snprintf(buf + n, sizeof buf - n, " %08X", v); ++found; }
    }
    if (readable(params, 0x50)) {
        const float* f = reinterpret_cast<const float*>(params);
        n += std::snprintf(buf + n, sizeof buf - n,
                           "; rect %.3f %.3f %.3f %.3f; view %.2f %.2f %.2f / %.2f %.2f %.2f / %.2f %.2f %.2f; position %.1f %.1f %.1f; fov %.3f",
                           f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11], f[12], f[16], f[17], f[18], f[19]);
    }
    fo::log("%s", buf);
}

void __fastcall hk_set(void* self, void* edx, const uint8_t* params, uint32_t a, uint32_t b) {
    const uint32_t ret = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__builtin_return_address(0)));
    for (Caller& c : g_callers) {
        if (c.ret == ret || c.ret == 0) {
            c.ret = ret;
            ++c.count;
            if (!c.logged && g_lines < 24) {
                c.logged = true;
                ++g_lines;
                log_first(ret, params);
            }
            break;
        }
    }
    o_set(self, edx, params, a, b);
}
}  // namespace

void install() {
    if (!readable(reinterpret_cast<const void*>(kFunc), sizeof kStart) ||
        std::memcmp(reinterpret_cast<const void*>(kFunc), kStart, sizeof kStart) != 0) {
        fo::log("camera: engine camera code not where expected, not watched");
        return;
    }
    uint8_t* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    std::memcpy(tramp, kStart, sizeof kStart);
    tramp[8] = 0xE9;
    const uint32_t back = (kFunc + 8) - (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(tramp)) + 13);
    std::memcpy(tramp + 9, &back, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, 13);
    o_set = reinterpret_cast<Set_t>(tramp);

    uint8_t jump[8] = {0xE9, 0, 0, 0, 0, 0x90, 0x90, 0x90};
    const uint32_t rel = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&hk_set)) - (kFunc + 5);
    std::memcpy(jump + 1, &rel, 4);
    DWORD old = 0;
    void* at = reinterpret_cast<void*>(kFunc);
    if (!VirtualProtect(at, sizeof jump, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(at, jump, sizeof jump);
    VirtualProtect(at, sizeof jump, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof jump);
}

void log_counts(unsigned updates) {
    if (!updates) {
        for (Caller& c : g_callers) c.count = 0;
        return;
    }
    char buf[400];
    int n = std::snprintf(buf, sizeof buf, "camera: engine camera sets over %u updates:", updates);
    bool any = false;
    for (Caller& c : g_callers) {
        if (!c.ret) break;
        if (c.count && n < 360) {
            n += std::snprintf(buf + n, sizeof buf - n, " %08X x%u", c.ret, c.count);
            any = true;
        }
        c.count = 0;
    }
    if (any) fo::log("%s", buf);
}
}  // namespace fo::display::camera_probe
