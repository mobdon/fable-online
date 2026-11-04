// The realm sky frame and the arithmetic that keeps Fable's clock on it (src/world/sky.cpp).
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "world/sky.h"

using namespace fo::world;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)
static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

int main() {
    // A frame exactly as the server writes it (JSON.stringify of WorldClock.sky()).
    const std::string frame =
        "{\"t\":\"sky\",\"day_fraction\":0.75,\"day\":12,\"day_ms\":2880000,\"night\":false,\"climate\":\"temperate\","
        "\"weather\":\"rain\",\"intensity\":0.62,\"previous\":\"cloudy\",\"since\":1000,\"until\":721000,\"fade_ms\":30000,"
        "\"server_time\":5000}";
    Sky s;
    CHECK(parse_sky(frame, 1000, s));
    CHECK(s.have && near(s.dayFraction, 0.75) && near(s.dayMs, 2880000));
    CHECK(s.weather == "rain" && s.previous == "cloudy" && s.climate == "temperate" && !s.night);
    CHECK(near(s.intensity, 0.62, 1e-5));
    Sky untouched;
    CHECK(!parse_sky("{\"t\":\"tick\",\"seq\":1}", 0, untouched) && !untouched.have);
    CHECK(!parse_sky("{\"t\":\"sky\",\"day_fraction\":2,\"day_ms\":2880000}", 0, untouched));
    CHECK(!parse_sky("{\"t\":\"sky\",\"day_fraction\":0.5}", 0, untouched));

    // The time runs on at the realm's speed: an hour of game time is 2 real minutes here.
    CHECK(near(sky_fraction(s, 1000), 0.75));
    CHECK(near(sky_fraction(s, 1000 + 120000), 0.75 + 1.0 / 24));
    // Past midnight it wraps, and so does the tick count.
    s.dayFraction = 0.99;
    CHECK(near(sky_fraction(s, 1000 + 57600), 0.01, 1e-9));
    s.receivedMs = 0xFFFFFF00u;
    CHECK(near(sky_fraction(s, 0x100u), 0.99 + 512.0 / 2880000.0, 1e-9));

    CHECK(near(day_delta(0.9, 0.1), 0.2));
    CHECK(near(day_delta(0.1, 0.9), -0.2));
    CHECK(near(day_delta(0.3, 0.3), 0.0));
    int h = 0, m = 0;
    day_hm(0.75, h, m);
    CHECK(h == 18 && m == 0);
    day_hm(0.99999, h, m);
    CHECK(h == 0 && m == 0);

    const double day = 2880000, base = 1000.0 / day;
    // In step: run at the realm's speed.
    ClockStep st = clock_step(0.5, 0.5, day);
    CHECK(!st.snap && near(st.speed, base, 1e-12));
    // A little behind: run faster to catch up over kCatchUpSeconds, without a jump.
    st = clock_step(0.5, 0.505, day);
    CHECK(!st.snap && near(st.speed, base + 0.005 / kCatchUpSeconds, 1e-12));
    // A little ahead: slow down, never run backwards.
    st = clock_step(0.505, 0.5, day);
    CHECK(!st.snap && st.speed == 0.0);
    st = clock_step(0.5001, 0.5, day);
    CHECK(!st.snap && st.speed > 0 && st.speed < base);
    // Far out (a join, a load, a staff change): snap, also across midnight.
    st = clock_step(0.3, 0.9, day);
    CHECK(st.snap && near(st.fraction, 0.9) && near(st.speed, base, 1e-12));
    st = clock_step(0.995, 0.005, day);
    CHECK(!st.snap && st.speed > base);   // 0.01 ahead through midnight: a catch-up, not a snap
    st = clock_step(NAN, 0.5, day);
    CHECK(st.snap);

    // Weather: rain and snow are drawn, the rest leaves the area its own weather.
    Sky w;
    w.have = true;
    w.climate = "temperate";
    w.weather = "rain";
    w.intensity = 0.5f;
    Precip p = precip_for(w);
    CHECK(near(p.rain, 0.55, 1e-6) && p.snow == 0.0f);
    w.weather = "storm";
    CHECK(precip_for(w).rain == 1.0f);
    w.weather = "blizzard";
    p = precip_for(w);
    CHECK(p.snow == 1.0f && p.rain == 0.0f);
    w.weather = "fog";
    p = precip_for(w);
    CHECK(p.rain < 0 && p.snow < 0);
    w.weather = "rain";
    w.climate = "indoor";
    p = precip_for(w);
    CHECK(p.rain < 0 && p.snow < 0);
    CHECK(precip_for(Sky{}).rain < 0);

    if (failures) {
        std::printf("sky: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("sky: all passed\n");
    return 0;
}
