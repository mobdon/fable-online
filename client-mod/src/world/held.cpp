#include "held.h"

#include <cstdlib>

namespace fo::world {

LevelNames parse_wld_levels(const std::string& text) {
    LevelNames out;
    std::string script;
    uint32_t uid = 0;
    bool inMap = false;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t s = 0;
        while (s < line.size() && (line[s] == ' ' || line[s] == '\t')) ++s;
        line.erase(0, s);
        if (line.compare(0, 7, "NewMap ") == 0) {
            inMap = true;
            script.clear();
            uid = 0;
        } else if (line.compare(0, 7, "EndMap;") == 0) {
            if (inMap && uid && !script.empty()) out[uid] = script;
            inMap = false;
        } else if (inMap && line.compare(0, 16, "LevelScriptName ") == 0) {
            size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) script = line.substr(a + 1, b - a - 1);
        } else if (inMap && line.compare(0, 7, "MapUID ") == 0) {
            uid = static_cast<uint32_t>(std::strtoul(line.c_str() + 7, nullptr, 10));
        }
    }
    return out;
}

std::string server_thing_id(uint64_t uid, const LevelNames& levels) {
    auto it = levels.find(static_cast<uint32_t>(uid >> 40));
    if (it == levels.end()) return std::string();
    return thing_id(it->second, uid);
}

uint64_t runtime_uid(const std::string& id, const LevelNames& levels) {
    size_t colon = id.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= id.size()) return 0;
    std::string level = id.substr(0, colon);
    char* end = nullptr;
    unsigned long long low = std::strtoull(id.c_str() + colon + 1, &end, 10);
    if (!end || *end != '\0' || low > 0xFFFFFFFFull) return 0;
    for (const auto& l : levels)
        if (l.second == level) return (static_cast<uint64_t>(l.first) << 40) | low;
    return 0;
}

bool HeldActions::hold(uintptr_t token, HeldKind kind, const std::string& id, uint32_t nowMs) {
    if (held_.count(token)) return false;
    held_[token] = Entry{kind, id, nowMs};
    return true;
}

void HeldActions::decide(uintptr_t token, Verdict v, const std::string& why) {
    auto it = held_.find(token);
    if (it == held_.end()) return;
    out_.push_back(Decision{token, it->second.kind, it->second.id, v, why});
    held_.erase(it);
}

void HeldActions::on_region_change() {
    std::vector<uintptr_t> exits;
    for (const auto& h : held_)
        if (h.second.kind == HeldKind::Exit) exits.push_back(h.first);
    for (uintptr_t t : exits) decide(t, Verdict::Release, "moved");
}

void HeldActions::on_loot(const Loot& loot) {
    std::vector<uintptr_t> hit;
    for (const auto& h : held_)
        if (h.second.kind == HeldKind::Give && h.second.id == loot.thing) hit.push_back(h.first);
    // "again": this hero already had it, so the local game must not hand it over twice.
    for (uintptr_t t : hit) decide(t, loot.again ? Verdict::Drop : Verdict::Release, loot.again ? "again" : "loot");
}

bool HeldActions::refusal_blocks(HeldKind kind, const std::string& code) {
    // A rule said no: the hero may not go or take. Any other refusal (too far, not in this
    // region, still arriving) means the server could not check, and the game goes ahead so
    // nobody is stuck at a door; the server's ledger stays the record of what was taken.
    if (code == "demon_door_closed" || code == "locked" || code == "jailed") return true;
    if (kind == HeldKind::Give)
        return code == "gone" || code == "need_keys" || code == "need_spade" || code == "for_sale";
    return false;
}

void HeldActions::on_refusal(const Refusal& r) {
    if (r.thing.empty()) return;
    std::vector<std::pair<uintptr_t, HeldKind>> hit;
    for (const auto& h : held_)
        if (h.second.id == r.thing) hit.push_back({h.first, h.second.kind});
    for (const auto& h : hit) decide(h.first, refusal_blocks(h.second, r.code) ? Verdict::Drop : Verdict::Release, r.code);
}

void HeldActions::on_ok(const std::string& id) {
    // An accepted exit is followed by enter_region; release on whichever comes first.
    std::vector<uintptr_t> hit;
    for (const auto& h : held_)
        if (h.second.id == id && h.second.kind == HeldKind::Exit) hit.push_back(h.first);
    for (uintptr_t t : hit) decide(t, Verdict::Release, "ok");
}

void HeldActions::release_all(const char* why) {
    std::vector<uintptr_t> all;
    for (const auto& h : held_) all.push_back(h.first);
    for (uintptr_t t : all) decide(t, Verdict::Release, why ? why : "released");
}

std::vector<Decision> HeldActions::drain(uint32_t nowMs) {
    std::vector<uintptr_t> late;
    for (const auto& h : held_) {
        uint32_t wait = h.second.kind == HeldKind::Exit ? kExitWaitMs : kGiveWaitMs;
        if (nowMs - h.second.since >= wait) late.push_back(h.first);
    }
    for (uintptr_t t : late) decide(t, Verdict::Release, "timeout");
    std::vector<Decision> out;
    out.swap(out_);
    return out;
}

}  // namespace fo::world
