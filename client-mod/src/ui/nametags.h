#pragma once
#include <array>
#include <map>
// Name tags above other heroes: the server sends who is near and where ("tags" messages);
// each frame this projects them onto the screen with the camera's view-projection matrix.
// No Windows or Direct3D in here (tested on Linux).
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "json.h"

namespace fo::ui {

struct Tag {
    uint32_t id = 0;
    std::string name, title, guild;
    double renown = 0;
    bool pvp = false;
    std::string staff;                // "DEV" / "GM" from the server's account role, "" for players
    float x = 0, y = 0, z = 0;        // last position from the server
    float px = 0, py = 0, pz = 0;     // where we draw it: eased toward x/y/z
};

struct Label {
    float x, y;                        // screen pixels: centre of the name line
    std::string name_line;             // "Hal <GLD>"
    std::string info_line;             // "Arrow Master - 1,250 renown"
    uint32_t name_color, info_color;   // 0xAARRGGBB, alpha already applied
    float depth;                       // view distance, for sorting
    bool pvp;                          // flagged for open-world PvP
    bool self = false;                 // the player's own hero: drawn on a gold plate
    std::string badge;                 // staff badge above the name ("DEV", "GM"), "" for none
};

struct TagSettings {
    float head_height = 2.1f;          // tags float this far above a hero's position
    int up_axis = 2;                   // 0 = x, 1 = y, 2 = z: which world axis points up
    float full_until = 25.0f;          // fully visible up to this distance
    float hidden_from = 55.0f;         // invisible from this distance
    float line_height = 16.0f;         // pixels, from the font
};

// Colour by renown, so famous heroes stand out. Thresholds are a first pass; balance later.
// Fable's fonts only cover ASCII, so tags stick to it.
uint32_t renown_color(double renown);
std::string format_renown(double renown);   // 12500 -> "12,500 renown"

class TagStore {
public:
    // Applies a "tags" message. A new region means all old tags are gone.
    void apply(const json::Value& m);
    void clear() { tags_.clear(); }
    // Dev check: put a tag on your own hero so placement can be checked solo.
    void set_self(bool on, const std::string& name, double renown, float x, float y, float z,
                  const std::string& staff = "");
    // Where the stand-ins are drawn right now (hero id -> position). Tags on those heroes
    // sit on the stand-in instead of easing toward the chat service's slower updates.
    void set_live(std::map<uint32_t, std::array<float, 3>> live) { live_ = std::move(live); }

    // Eases drawn positions toward the latest server positions (call once a frame).
    void step(float dt);

    // view_proj: row-major D3D matrix (row vectors: clip = [x y z 1] * M).
    std::vector<Label> layout(const float view_proj[16], float screen_w, float screen_h,
                              const TagSettings& s) const;

    size_t size() const { return tags_.size(); }
    const std::map<uint32_t, Tag>& tags() const { return tags_; }

private:
    std::map<uint32_t, Tag> tags_;
    std::map<uint32_t, std::array<float, 3>> live_;
    uint32_t region_ = 0xFFFFFFFF;
    bool self_on_ = false;
    Tag self_;
};

}  // namespace fo::ui
