#include "gear_report.h"

namespace fo::world {

bool valid_def_name(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (char c : s)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

void GearReporter::reset() {
    haveSent_ = false;
    haveSeen_ = false;
}

std::string GearReporter::update(const std::string& melee, const std::string& ranged, uint32_t nowMs) {
    const std::string m = valid_def_name(melee) ? melee : std::string();
    const std::string r = valid_def_name(ranged) ? ranged : std::string();
    if (!haveSeen_ || m != seenMelee_ || r != seenRanged_) {
        haveSeen_ = true;
        seenMelee_ = m;
        seenRanged_ = r;
        seenSince_ = nowMs;
    }
    const bool same = haveSent_ && m == sentMelee_ && r == sentRanged_;
    if (same && nowMs - sentAt_ < kGearResendMs) return std::string();
    // A first report goes at once; a change waits until it has held for a moment.
    if (!same && haveSent_ && nowMs - seenSince_ < kGearSettleMs) return std::string();
    haveSent_ = true;
    sentMelee_ = m;
    sentRanged_ = r;
    sentAt_ = nowMs;
    return "{\"t\":\"gear\",\"melee\":\"" + m + "\",\"ranged\":\"" + r + "\"}";
}

}  // namespace fo::world
