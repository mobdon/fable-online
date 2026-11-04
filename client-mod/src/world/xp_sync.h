#pragma once
// Hero experience: the server's copy is the truth (server/src/heroes/experience.ts), and the
// local game is kept in step with it. Pure logic, unit-tested in client-mod/tests/world_test.cpp;
// reading and writing Fable's memory is in game/hero_xp.cpp.
//
// The loop:
//  - The mod reads Fable's pools and levels a few times a second. When they differ from what
//    it last wrote (Fable bought a level, a quest paid experience), and have held still for a
//    moment, it sends them to the server (xp_sync), naming the state they grew from (`base`).
//  - The server prices any new level from Fable's own tables, takes local gains up to a cap,
//    and answers with its state (xp_state). It also sends xp_state after every server kill.
//  - The mod writes the server's state into Fable. A refused purchase or an edited save is
//    undone that way.
#include <cstdint>
#include <string>

namespace fo::world {

constexpr int kXpPools = 4;      // general, strength, skill, will
constexpr int kXpStats = 10;     // Fable's TrainableStatLevels
constexpr int kXpAbilities = 20; // CTCInventoryAbilities slots

/** Experience as the server counts it. Ability levels are 0 (not owned) to 4. */
struct XpSnapshot {
    int32_t pools[kXpPools] = {};
    int32_t total = 0;
    int32_t stats[kXpStats] = {};
    int32_t abilities[kXpAbilities] = {};
};
/** Same pools and levels. `total` is the server's alone (Fable only shows it). */
bool same_xp(const XpSnapshot& a, const XpSnapshot& b);

struct XpState {
    double seq = 0;
    XpSnapshot s;
    /** "sync" / "import" (an answer to this mod's report), "kill", "admin", ... */
    std::string why;
    int bought = 0;
    int refused = 0;
};

/** Decodes an xp_state frame. False if it is not one or is malformed. */
bool parse_xp_state(const std::string& json, XpState& out);
/** The xp_sync frame for a local snapshot. */
std::string xp_sync_frame(const XpSnapshot& local, double base);

/** Local changes must hold still this long before they are reported (a purchase lands over a few frames). */
constexpr uint32_t kXpSettleMs = 600;
/** A report with no answer after this long may be sent again. */
constexpr uint32_t kXpAnswerMs = 5000;

class XpSync {
  public:
    /**
     * Once a tick, with what Fable has now. Returns true and fills `frame` when a report
     * should go to the server.
     */
    bool step(const XpSnapshot& local, uint32_t nowMs, std::string& frame);
    /** An xp_state from the server. */
    void on_state(const XpState& st);
    /**
     * True when the server's state should be written into Fable now (fills `out`). Never
     * while a report waits for its answer, so a purchase isn't undone before it is checked.
     */
    bool take_write(const XpSnapshot& local, XpSnapshot& out);
    /** After writing `take_write`'s state into Fable. */
    void wrote();
    /** Connection or hero changed: start over with a first report. */
    void reset() { *this = XpSync(); }

    bool have_server() const { return haveServer_; }
    double server_seq() const { return server_.seq; }

  private:
    bool haveServer_ = false;
    bool pending_ = false;
    bool forceWrite_ = false;
    bool sentFirst_ = false;
    uint32_t sentMs_ = 0;
    uint32_t changedMs_ = 0;
    bool haveLast_ = false;
    XpSnapshot last_;
    XpSnapshot applied_;
    double appliedSeq_ = 0;
    XpState server_;
};

}  // namespace fo::world
