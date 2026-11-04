#pragma once
// Parsing of the server's world frames. No Windows or game dependencies, so this is
// unit-tested on its own (client-mod/tests/world_test.cpp).
//
// Frames are the JSON objects described in shared/protocol/messages.proto. Only the
// fields the mod needs are read; anything else in a frame is ignored, so the server can
// add fields without breaking older clients.
#include <cstdint>
#include <string>
#include <vector>

namespace fo::world {

/** What the server says an entity is doing. Mirrors the server's Anim enum. */
enum class Anim : uint8_t {
    Idle = 0,
    Walk = 1,
    Run = 2,
    Attack = 3,
    Alert = 4,
    Hit = 5,
    Death = 6,
    Leap = 7,
    Strafe = 8,
    Block = 9,
    Taunt = 10,
};

enum class Kind : uint8_t { Hero, Creature };

/** One entity in one frame, in the region's local metres (z up), as the server sends it. */
struct EntityState {
    uint32_t id = 0;
    Kind kind = Kind::Creature;
    std::string def;   // creatures: Fable def name, e.g. CREATURE_BANDIT_GRUNT_LEVEL1
    std::string name;  // heroes: display name for the name tag
    float x = 0, y = 0, z = 0;
    float yaw = 0;
    Anim anim = Anim::Idle;
    float health = 0;
    uint32_t renown = 0;
};

/** A decoded `tick` frame. */
struct Tick {
    uint32_t tick = 0;
    uint32_t lastInputSeq = 0;
    std::vector<EntityState> entities;
};

/** A decoded `welcome` frame: tells the mod which entity id is its own hero. */
struct Welcome {
    uint32_t playerId = 0;
    std::string name;
    uint32_t tickRate = 15;
    /** One-time token to reconnect with after this connection drops (no new launcher ticket needed). */
    std::string resume;
    /** What the server does with position reports: "off", "checked" or "trusted". */
    std::string clientMove = "off";
};

/** The server refused a position report and says where it has the hero. */
struct PosCorrect {
    float x = 0, y = 0, z = 0;
    std::string reason;
};

enum class FrameType { Unknown, Tick, Welcome, EnterRegion, Error, PosCorrect, Pong };

FrameType frame_type(const std::string& json);
/** Entities with a non-finite or absurd coordinate are dropped, not clamped. */
bool parse_tick(const std::string& json, Tick& out);
bool parse_welcome(const std::string& json, Welcome& out);
/** `region` of an enter_region frame, plus the spawn point the server picked. */
bool parse_enter_region(const std::string& json, std::string& region, float& x, float& y, float& z);
bool parse_pos_correct(const std::string& json, PosCorrect& out);
/** The `n` of a pong frame: whatever the matching ping sent, here the send time in ms. */
bool parse_pong(const std::string& json, double& n);
/** The `error`/`message` pair of an error frame. */
bool parse_error(const std::string& json, std::string& code, std::string& message);

}  // namespace fo::world
