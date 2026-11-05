#include "weather.h"

#include <windows.h>

#include <cmath>
#include <cstdint>

namespace fo::game::weather {
namespace {

// Fable.exe has no ASLR, so these are fixed. See tools/re-notes/08-world-time.md.
constexpr uintptr_t kRendererGlobal = 0x01436E44;
constexpr uint32_t kRendererVtable = 0x012A2434;   // CEngineWeatherRenderer
constexpr uint32_t kRain = 0x5C8;                  // float, rain being drawn
constexpr uint32_t kSnow = 0x5CC;                  // float, snow being drawn
constexpr uint32_t kRainAmount = 0x12E8;           // float, override amount
constexpr uint32_t kRainOn = 0x12EC;               // byte, override on
constexpr uint32_t kSnowAmount = 0x12F0;
constexpr uint32_t kSnowOn = 0x12F4;
constexpr uint32_t kMax = 0x12F8;                  // float, most the renderer draws (1.0)

bool rd(uintptr_t a, void* out, size_t n) {
    SIZE_T got = 0;
    return a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(a), out, n, &got) && got == n;
}

bool wr(uintptr_t a, const void* in, size_t n) {
    SIZE_T put = 0;
    return a && WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<LPVOID>(a), in, n, &put) && put == n;
}

uintptr_t renderer() {
    uint32_t r = 0, vt = 0;
    if (!rd(kRendererGlobal, &r, 4) || !rd(r, &vt, 4) || vt != kRendererVtable) return 0;
    return r;
}

// The same as Fable's own setters (0x00B50EE0 rain, 0x00B50F40 snow): negative turns the
// override off, otherwise it is capped at the renderer's maximum.
bool set(uint32_t amountAt, uint32_t onAt, float amount) {
    uintptr_t r = renderer();
    if (!r || !std::isfinite(amount)) return false;
    uint8_t on = amount >= 0.0f ? 1 : 0;
    if (on) {
        float most = 1.0f;
        if (rd(r + kMax, &most, 4) && std::isfinite(most) && most > 0.0f && amount > most) amount = most;
        if (!wr(r + amountAt, &amount, 4)) return false;
    }
    return wr(r + onAt, &on, 1);
}

}  // namespace

bool read(State& out) {
    uintptr_t r = renderer();
    State s;
    uint8_t rainOn = 0, snowOn = 0;
    float rainAmt = 0, snowAmt = 0;
    if (!r || !rd(r + kRain, &s.rain, 4) || !rd(r + kSnow, &s.snow, 4) || !rd(r + kRainOn, &rainOn, 1) ||
        !rd(r + kSnowOn, &snowOn, 1) || !rd(r + kRainAmount, &rainAmt, 4) || !rd(r + kSnowAmount, &snowAmt, 4))
        return false;
    s.found = true;
    s.rainOn = rainOn != 0;
    s.snowOn = snowOn != 0;
    float most = 1.0f;
    if (rd(r + kMax, &most, 4) && std::isfinite(most) && most > 0.0f) s.most = most;
    s.rainOverride = rainOn ? rainAmt : -1.0f;
    s.snowOverride = snowOn ? snowAmt : -1.0f;
    out = s;
    return true;
}

bool set_rain(float amount) { return set(kRainAmount, kRainOn, amount); }
bool set_snow(float amount) { return set(kSnowAmount, kSnowOn, amount); }

}  // namespace fo::game::weather
