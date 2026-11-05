#pragma once
// The mod's side of doors, chests, items on the ground and region changes
// (docs/world-objects.md). The server decides who opens or takes what; this keeps the
// server's answers and tells the game side what to change.
//
// Pure logic, no Windows or game calls, so it is unit-tested (client-mod/tests/objects_test.cpp).
// client-mod/src/game/things.h applies the changes to Fable's own things.
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fo::world {

/**
 * The server's id for a thing: "<LevelScriptName>:<uid & 0xFFFFFFFF>". Fable's thing
 * UIDs repeat between levels, so the level is part of it.
 */
std::string thing_id(const std::string& level, uint64_t uid);

/** {"t":"interact",...}. `action` is "use", "open", "close" or "opened" (demon doors). */
std::string interact_frame(const std::string& thing, const char* action = "use");
/** {"t":"region_ready",...}: the local game finished loading `region`. */
std::string region_ready_frame(const std::string& region);

/** The fields of an enter_region frame that matter for a region change. */
struct RegionChange {
    std::string region;
    std::string from;      // empty on the first region after joining
    std::string via;       // "exit", "door", "cullis_gate"; empty on join
    std::string entrance;  // thing id of the entrance the hero arrives at
    std::string thing;     // thing id of the exit or door used
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    bool hasYaw = false;
    /** The server holds the hero until it gets region_ready. */
    bool loading = false;
};
bool parse_region_change(const std::string& json, RegionChange& out);

struct LootItem {
    std::string itemId;  // the server's unique id for this item
    std::string def;
};

struct Loot {
    std::string thing;
    uint32_t gold = 0;
    std::vector<LootItem> items;
    std::vector<std::string> blocked;
    /** The hero had already taken it; nothing new was made. */
    bool again = false;
};

struct Refusal {
    std::string thing;  // empty when the error was not about a thing
    std::string code;   // too_far, need_keys, gone, locked, demon_door_closed, ...
    std::string message;
};

/** What the game side has to change on a thing. */
struct ObjectChange {
    enum class What : uint8_t { DoorOpen, DoorClosed, Gone, Back, Opened };
    std::string id;
    What what;
};

enum class ObjectFrame { None, Objects, Object, Loot, Refused, Ok };

/** A request for the same thing is not sent again while the last one is unanswered for this long. */
constexpr uint32_t kRequestRetryMs = 1000;
/** How long after a region change the mod waits for the local load before telling the server anyway. */
constexpr uint32_t kLoadGiveUpMs = 25000;

class WorldObjects {
  public:
    /**
     * Feed any server frame. Frames that are not about things return None and change
     * nothing, so every frame can go through here.
     */
    ObjectFrame on_frame(const std::string& json, uint32_t nowMs);

    /** A door's state: the server's if it said so, else the level file's default. */
    bool door_open(const std::string& id, bool levelDefault) const;
    bool gone(const std::string& id) const { return gone_.count(id) != 0; }
    bool opened(const std::string& id) const { return opened_.count(id) != 0; }

    std::vector<ObjectChange> drain_changes();
    std::vector<Loot> drain_loot();
    std::vector<Refusal> drain_refusals();
    /** Thing ids the server accepted with a plain {"t":"ok","thing":...}. */
    std::vector<std::string> drain_acks();

    /**
     * The hero tried to use a thing. Fills `frame` with what to send and returns true, or
     * returns false when a request for that thing is still waiting for its answer.
     */
    bool request(const std::string& id, const char* action, uint32_t nowMs, std::string& frame);

    /**
     * A region change started (an enter_region with loading set). Forgets every thing
     * of the old region; the server sends the new region's state in an objects frame.
     */
    void begin_region(const RegionChange& rc, uint32_t nowMs);
    bool loading() const { return loading_; }
    const RegionChange& region() const { return region_; }
    /**
     * The local game finished loading (or the wait gave up). Returns true once, with the
     * region_ready frame to send.
     */
    bool finish_loading(uint32_t nowMs, bool loaded, std::string& frame);

    void clear();

  private:
    std::unordered_map<std::string, bool> doors_;
    std::unordered_set<std::string> gone_;
    std::unordered_set<std::string> opened_;
    std::unordered_map<std::string, uint32_t> pending_;
    std::vector<ObjectChange> changes_;
    std::vector<Loot> loot_;
    std::vector<Refusal> refusals_;
    std::vector<std::string> acks_;
    RegionChange region_;
    bool loading_ = false;
    uint32_t loadingSinceMs_ = 0;
};

}  // namespace fo::world
