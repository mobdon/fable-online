#pragma once
// Reads the local hero's position from Fable.exe memory (offsets: tools/re-notes/02-hero-live-state.md).
// Interim until the region server owns positions: the mod reports it so name tags work in dev.
#include <cstdint>

namespace fo::game {
struct HeroPos {
    bool valid = false;
    float x = 0, y = 0, z = 0;
    uint32_t area = 0;   // stand-in region id: a hash of the area anchor at hero+0xE8 (region UID not mapped yet)
};
// Call from a background thread a few times a second. Scans for the hero when it's lost.
HeroPos hero_read();
}  // namespace fo::game
