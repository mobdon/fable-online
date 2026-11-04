#pragma once
// The party as the mod shows it: who is in it, their colours, and the orbs over party
// members' stand-ins. The world server owns parties (server/src/party/parties.ts); this only
// keeps its last word, turns party frames into chat lines, and lays the orbs out on screen.
// No Windows or Direct3D in here (tested on Linux).
#include <cstdint>
#include <string>
#include <vector>
#include "chat_model.h"
#include "json.h"

namespace fo::ui {

struct PartyMember {
    uint32_t id = 0;
    std::string name;
    int slot = 0;                  // 0..3: red, blue, yellow, green (Fable's co-op colours)
    bool leader = false;
    bool online = true;
    std::string region;
};

// Where a party member's stand-in is this frame, from the entity table (game thread).
struct PartyMark {
    uint32_t id = 0;
    int slot = 0;
    float x = 0, y = 0, z = 0;
};

struct Orb {
    float x, y;                    // screen pixels, centre
    float radius;                  // pixels, smaller with distance
    uint32_t color;                // 0xAARRGGBB, alpha already applied
    float depth;
};

class PartyModel {
public:
    // A frame from the world server. Party frames update the list and/or add chat lines;
    // returns false for anything else.
    bool on_world(const json::Value& m, ChatModel& chat, double now);

    bool in_party() const { return id_ != 0; }
    uint32_t leader() const { return leader_; }
    const std::vector<PartyMember>& members() const { return members_; }
    // Hero name of the newest invite still waiting, or "".
    const std::string& invite_from() const { return invite_from_; }

    // Colours of the four co-op spirits, as Fable's MULTIPLAYER_<colour> glows tint them.
    static uint32_t slot_color(int slot);
    static const char* slot_name(int slot);

    void set_marks(std::vector<PartyMark> marks) { marks_ = std::move(marks); }
    // Orbs over party members' heads. view_proj and up_axis as for name tags.
    std::vector<Orb> layout(const float view_proj[16], float screen_w, float screen_h, int up_axis,
                            float head_height) const;

private:
    uint32_t id_ = 0;
    uint32_t leader_ = 0;
    std::vector<PartyMember> members_;
    std::string invite_from_;
    std::vector<PartyMark> marks_;
};

// Reads the member list out of a "party" frame: (hero id, colour slot) for each member.
// Used by the game thread for the spirit markers.
std::vector<std::pair<uint32_t, int>> party_slots(const json::Value& m);

}  // namespace fo::ui
