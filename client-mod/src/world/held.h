#pragma once
// Fable actions the mod holds back until the server has answered (docs/world-objects.md).
//
// Three of Fable's own actions are held:
//  - A region exit firing. Fable would start loading the next region at once; online, the
//    mod asks the server first and lets the load start when the server moves the hero.
//  - A chest, cupboard or digging spot handing its contents to the hero. The server keeps
//    the ledger, so the local game only hands them over when the server says this hero
//    gets them now (and not "again").
//  - A Cullis Gate or Guild Seal teleport (and the teleport screen's "go back"). The server
//    owns which gates a hero has found and when they may travel; Fable jumps when the
//    server has moved the hero.
//
// Pure logic, no Windows or game calls, so it is unit-tested (client-mod/tests/held_test.cpp).
// Also here: the thing-id helpers that turn Fable's 64-bit UIDs into the server's ids.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "objects.h"

namespace fo::world {

/**
 * Map UID -> level script name, read from the player's own Data/Levels/FinalAlbion.wld
 * (`NewMap ... LevelScriptName "X"; MapUID n; ... EndMap;`).
 */
using LevelNames = std::unordered_map<uint32_t, std::string>;
LevelNames parse_wld_levels(const std::string& wldText);

/**
 * A thing's UID while the game runs is (MapUID << 40) | (low 40 bits from the level file).
 * Returns the server id "<LevelScriptName>:<uid & 0xFFFFFFFF>", or "" when the map is not
 * one of the level files (things made at run time, global things).
 */
std::string server_thing_id(uint64_t uid, const LevelNames& levels);
/** The reverse, for the server's ids: the run-time UID of `id`, or 0 if the level is unknown. */
uint64_t runtime_uid(const std::string& id, const LevelNames& levels);

enum class HeldKind : uint8_t { Exit, Give, Teleport, TeleportBack };
enum class Verdict : uint8_t { Release, Drop };

/** How long a held action waits for the server before the game is let go anyway. */
constexpr uint32_t kExitWaitMs = 3000;
constexpr uint32_t kGiveWaitMs = 5000;
constexpr uint32_t kTeleportWaitMs = 4000;

/** {"t":"travel","to":...}: `to` is a gate's thing id or "back". */
std::string travel_frame(const std::string& to);

struct Decision {
    uintptr_t token;  // what the game side passed to hold(): the component pointer
    HeldKind kind;
    std::string id;
    Verdict verdict;
    /** Why, for the log: "moved", "loot", "again", "timeout", the server's error code, ... */
    std::string why;
};

class HeldActions {
  public:
    /** Returns false (and holds nothing) if this token is already held. */
    bool hold(uintptr_t token, HeldKind kind, const std::string& id, uint32_t nowMs);
    bool held(uintptr_t token) const { return held_.count(token) != 0; }
    size_t size() const { return held_.size(); }

    /** The server moved the hero to another region: every held exit goes ahead. */
    void on_region_change();
    /** The server answered a request for `id` with loot. */
    void on_loot(const Loot& loot);
    /** The server refused a request for `id`. */
    void on_refusal(const Refusal& r);
    /** The server accepted a request for `id` with a plain {"t":"ok"} (or a travel_result ok). */
    void on_ok(const std::string& id);
    /** Lost the server: let the game carry on by itself. */
    void release_all(const char* why);

    /** Decisions made since the last call, timeouts included. */
    std::vector<Decision> drain(uint32_t nowMs);

    /** The rule for a refusal: true means the game must not go ahead. */
    static bool refusal_blocks(HeldKind kind, const std::string& code);

  private:
    struct Entry {
        HeldKind kind;
        std::string id;
        uint32_t since;
    };
    void decide(uintptr_t token, Verdict v, const std::string& why);
    std::unordered_map<uintptr_t, Entry> held_;
    std::vector<Decision> out_;
};

}  // namespace fo::world
