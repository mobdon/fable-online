#include "gear_report.h"

namespace fo::world {

bool valid_def_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

namespace {
GearSeen cleaned(const GearSeen& in) {
    GearSeen g;
    g.melee = valid_def_name(in.melee) ? in.melee : std::string();
    g.ranged = valid_def_name(in.ranged) ? in.ranged : std::string();
    g.armourKnown = in.armourKnown;
    if (in.armourKnown)
        for (const std::string& a : in.armour)
            if (valid_def_name(a) && g.armour.size() < 12) g.armour.push_back(a);
    return g;
}
}  // namespace

std::string gear_frame(const GearSeen& in) {
    const GearSeen g = cleaned(in);
    std::string f = "{\"t\":\"gear\",\"melee\":\"" + g.melee + "\",\"ranged\":\"" + g.ranged + "\"";
    if (g.armourKnown) {
        f += ",\"armour\":[";
        for (size_t i = 0; i < g.armour.size(); ++i) f += (i ? ",\"" : "\"") + g.armour[i] + "\"";
        f += "]";
    }
    return f + "}";
}

void GearReporter::reset() {
    haveSent_ = false;
    haveSeen_ = false;
}

std::string GearReporter::update(const GearSeen& raw, uint32_t nowMs) {
    const GearSeen g = cleaned(raw);
    if (!haveSeen_ || g != seen_) {
        haveSeen_ = true;
        seen_ = g;
        seenSince_ = nowMs;
    }
    const bool same = haveSent_ && g == sent_;
    if (same && nowMs - sentAt_ < kGearResendMs) return std::string();
    // A first report goes at once; a change waits until it has held for a moment.
    if (!same && haveSent_ && nowMs - seenSince_ < kGearSettleMs) return std::string();
    haveSent_ = true;
    sent_ = g;
    sentAt_ = nowMs;
    return gear_frame(g);
}

}  // namespace fo::world
