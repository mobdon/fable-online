#include "party_model.h"
#include <algorithm>
#include <cmath>

namespace fo::ui {

namespace {
uint32_t with_alpha(uint32_t c, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    return (static_cast<uint32_t>(static_cast<float>(c >> 24) * a) << 24) | (c & 0x00FFFFFF);
}
}  // namespace

uint32_t PartyModel::slot_color(int slot) {
    switch (slot) {
    case 0: return 0xFFE0573F;   // red
    case 1: return 0xFF5E93EE;   // blue
    case 2: return 0xFFF2CF4E;   // yellow
    case 3: return 0xFF72C866;   // green
    default: return 0xFFE9E4D6;
    }
}

const char* PartyModel::slot_name(int slot) {
    static const char* const kNames[] = {"red", "blue", "yellow", "green"};
    return slot >= 0 && slot < 4 ? kNames[slot] : "";
}

std::vector<std::pair<uint32_t, int>> party_slots(const json::Value& m) {
    std::vector<std::pair<uint32_t, int>> out;
    if (m.str("t") != "party") return out;
    for (const auto& v : m["members"].arr) {
        if (!v.is_object()) continue;
        double id = v.num("id", 0), slot = v.num("slot", -1);
        if (id <= 0 || slot < 0 || slot > 3) continue;
        out.emplace_back(static_cast<uint32_t>(id), static_cast<int>(slot));
    }
    return out;
}

bool PartyModel::on_world(const json::Value& m, ChatModel& chat, double now) {
    const std::string t = m.str("t");
    if (t == "party") {
        id_ = m["id"].type == json::Value::Type::Number ? static_cast<uint32_t>(m.num("id")) : 0;
        leader_ = static_cast<uint32_t>(m.num("leader", 0));
        members_.clear();
        for (const auto& v : m["members"].arr) {
            if (!v.is_object()) continue;
            PartyMember pm;
            pm.id = static_cast<uint32_t>(v.num("id", 0));
            pm.name = v.str("name");
            pm.slot = std::clamp(static_cast<int>(v.num("slot", 0)), 0, 3);
            pm.leader = v.boolean("leader");
            pm.online = v.boolean("online", true);
            pm.region = v.str("region");
            members_.push_back(std::move(pm));
        }
        if (!id_) marks_.clear();
        if (id_) invite_from_.clear();
        return true;
    }
    if (t == "party_invite") {
        invite_from_ = m.str("name");
        std::string who = invite_from_.empty() ? std::string("Someone") : invite_from_;
        chat.party_note(who + " invites you to their party. Type /accept or /decline.", now);
        return true;
    }
    if (t == "party_chat") {
        chat.party_line(m.str("name"), m.str("text"), now);
        return true;
    }
    if (t == "party_note") {
        chat.party_note(m.str("text"), now);
        return true;
    }
    if (t == "party_error") {
        std::string text = m.str("message");
        chat.party_note(text.empty() ? "The party request was refused." : text, now, true);
        return true;
    }
    return false;
}

std::vector<Orb> PartyModel::layout(const float m[16], float w, float h, int up_axis, float head_height) const {
    std::vector<Orb> out;
    for (const PartyMark& k : marks_) {
        float p[3] = {k.x, k.y, k.z};
        p[std::clamp(up_axis, 0, 2)] += head_height;
        float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
        float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
        float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        if (cw < 0.1f) continue;
        float nx = cx / cw, ny = cy / cw;
        if (nx < -1.1f || nx > 1.1f || ny < -1.1f || ny > 1.1f) continue;
        // Party members stay marked further out than name tags: they are who you look for.
        constexpr float kFull = 40.0f, kGone = 120.0f;
        if (cw > kGone) continue;
        float a = cw > kFull ? 1.0f - (cw - kFull) / (kGone - kFull) : 1.0f;
        Orb o;
        o.x = (nx + 1.0f) * 0.5f * w;
        o.y = (1.0f - ny) * 0.5f * h;
        o.radius = std::clamp(h * 0.012f * 12.0f / std::max(cw, 4.0f), h * 0.004f, h * 0.012f);
        o.color = with_alpha(slot_color(k.slot), a);
        o.depth = cw;
        out.push_back(o);
    }
    std::sort(out.begin(), out.end(), [](const Orb& a, const Orb& b) { return a.depth > b.depth; });
    return out;
}

}  // namespace fo::ui
