#include "sky.h"

#include <cmath>

#include "../ui/json.h"

namespace fo::world {

bool parse_sky(const std::string& json, uint32_t nowMs, Sky& out) {
    // Cheap test first: this runs for every frame that reaches the unknown-frame branch.
    if (json.find("\"t\":\"sky\"") == std::string::npos) return false;
    fo::json::Value v;
    if (!fo::json::parse(json, v) || v.str("t") != "sky") return false;
    double f = v.num("day_fraction", -1), ms = v.num("day_ms", 0);
    if (!(f >= 0 && f <= 1) || !(ms >= 60000)) return false;
    Sky s;
    s.have = true;
    s.dayFraction = f;
    s.dayMs = ms;
    s.receivedMs = nowMs;
    s.night = v.boolean("night");
    s.climate = v.str("climate");
    s.weather = v.str("weather");
    s.previous = v.str("previous");
    s.intensity = static_cast<float>(v.num("intensity"));
    s.since = v.num("since");
    s.until = v.num("until");
    s.fadeMs = v.num("fade_ms");
    out = s;
    return true;
}

double sky_fraction(const Sky& s, uint32_t nowMs) {
    if (!s.have || s.dayMs <= 0) return 0;
    // Unsigned subtraction copes with the tick count wrapping.
    double f = s.dayFraction + static_cast<double>(static_cast<uint32_t>(nowMs - s.receivedMs)) / s.dayMs;
    return f - std::floor(f);
}

double day_delta(double from, double to) {
    double d = to - from;
    d -= std::floor(d);       // 0..1
    if (d > 0.5) d -= 1.0;    // (-0.5, 0.5]
    return d;
}

void day_hm(double fraction, int& hour, int& minute) {
    int m = static_cast<int>(std::lround((fraction - std::floor(fraction)) * 1440.0)) % 1440;
    hour = m / 60;
    minute = m % 60;
}

ClockStep clock_step(double gameFraction, double realmFraction, double dayMs) {
    ClockStep st;
    double base = dayMs > 0 ? 1000.0 / dayMs : 0;
    double d = day_delta(gameFraction, realmFraction);
    if (!(std::fabs(d) <= kSnapGap)) {   // also catches NaN from a bad read
        st.snap = true;
        st.fraction = realmFraction;
        st.speed = base;
        return st;
    }
    st.fraction = gameFraction;
    st.speed = base + d / kCatchUpSeconds;
    if (st.speed < 0) st.speed = 0;      // Fable's clock never runs backwards: hold still instead
    return st;
}

Precip precip_for(const Sky& s) {
    Precip p;
    if (!s.have || s.climate == "indoor") return p;
    float i = s.intensity < 0 ? 0.0f : s.intensity > 1 ? 1.0f : s.intensity;
    if (s.weather == "rain") p.rain = 0.3f + 0.5f * i;
    else if (s.weather == "storm") p.rain = 1.0f;
    else if (s.weather == "snow") p.snow = 0.3f + 0.5f * i;
    else if (s.weather == "blizzard") p.snow = 1.0f;
    // Rain stops where snow falls and the other way round, so a change of weather fades over.
    if (p.rain > 0) p.snow = 0;
    else if (p.snow > 0) p.rain = 0;
    return p;
}

}  // namespace fo::world
