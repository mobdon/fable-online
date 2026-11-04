#pragma once
// When to tell the server what the hero has equipped in Fable. Pure logic, unit-tested in
// client-mod/tests/world_test.cpp; the reading itself is game/gear.cpp.
//
// The server only uses gear it accepts (server/src/combat/client-gear.ts), so this is a claim,
// sent when it changes and again now and then (the server forgets on a disconnect).
#include <cstdint>
#include <string>
#include <vector>

namespace fo::world {

/** Resend the same gear this often, in case a frame was lost or the server restarted. */
constexpr uint32_t kGearResendMs = 30000;
/** A change must hold this long before it is sent: swapping gear passes through "none". */
constexpr uint32_t kGearSettleMs = 400;

struct GearSeen {
    std::string melee, ranged;  // def names, "" for none
    std::vector<std::string> armour;
    bool armourKnown = false;   // false: armour is left out of the frame
    bool operator==(const GearSeen& o) const {
        return melee == o.melee && ranged == o.ranged && armour == o.armour && armourKnown == o.armourKnown;
    }
    bool operator!=(const GearSeen& o) const { return !(*this == o); }
};

class GearReporter {
public:
    /** Forget what was sent: call on (re)connect so the next read is sent at once. */
    void reset();
    /** What was read this tick. Returns the frame to send, or "" when nothing needs sending yet. */
    std::string update(const GearSeen& seen, uint32_t nowMs);

private:
    bool haveSent_ = false, haveSeen_ = false;
    GearSeen sent_, seen_;
    uint32_t sentAt_ = 0, seenSince_ = 0;
};

/** True for a def name the server's protocol accepts: [A-Z0-9_], 1..64 characters. */
bool valid_def_name(const std::string& s);

/** The frame for `g`: invalid names become "" (weapons) or are dropped (armour). */
std::string gear_frame(const GearSeen& g);

}  // namespace fo::world
