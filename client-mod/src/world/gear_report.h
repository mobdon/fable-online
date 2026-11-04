#pragma once
// When to tell the server which weapons the hero has equipped in Fable. Pure logic, unit-tested
// in client-mod/tests/world_test.cpp; the reading itself is game/gear.cpp.
//
// The server only fights with a weapon it accepts (server/src/combat/client-gear.ts), so this is
// a claim, sent when it changes and again now and then (the server forgets on a disconnect).
#include <cstdint>
#include <string>

namespace fo::world {

/** Resend the same gear this often, in case a frame was lost or the server restarted. */
constexpr uint32_t kGearResendMs = 30000;
/** A change must hold this long before it is sent: swapping weapons passes through "none". */
constexpr uint32_t kGearSettleMs = 400;

class GearReporter {
public:
    /** Forget what was sent: call on (re)connect so the next read is sent at once. */
    void reset();
    /**
     * The weapons read this tick (def names, "" for none). Returns the frame to send, or ""
     * when nothing needs sending yet.
     */
    std::string update(const std::string& melee, const std::string& ranged, uint32_t nowMs);
    const std::string& sent_melee() const { return sentMelee_; }

private:
    bool haveSent_ = false;
    std::string sentMelee_, sentRanged_;
    uint32_t sentAt_ = 0;
    std::string seenMelee_, seenRanged_;
    uint32_t seenSince_ = 0;
    bool haveSeen_ = false;
};

/** True for a def name the server's protocol accepts: [A-Z0-9_], 1..64 characters. */
bool valid_def_name(const std::string& s);

}  // namespace fo::world
