#include "objects.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace fo::world {
namespace {

// A small JSON value reader. These frames are small and shallow; depth is capped so a
// hostile frame cannot recurse without end.
struct Value {
    enum class Type : uint8_t { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> items;                              // array
    std::vector<std::pair<std::string, Value>> fields;     // object

    const Value* get(const char* key) const {
        if (type != Type::Object) return nullptr;
        for (const auto& f : fields)
            if (f.first == key) return &f.second;
        return nullptr;
    }
    std::string str(const char* key) const {
        const Value* v = get(key);
        return v && v->type == Type::String ? v->s : std::string();
    }
    bool num(const char* key, double& out) const {
        const Value* v = get(key);
        if (!v || v->type != Type::Number || !std::isfinite(v->n)) return false;
        out = v->n;
        return true;
    }
    bool flag(const char* key) const {
        const Value* v = get(key);
        return v && v->type == Type::Bool && v->b;
    }
};

constexpr int kMaxDepth = 6;

class Parser {
  public:
    explicit Parser(const std::string& s) : s_(s) {}
    bool parse(Value& out) { return value(out, 0) && (ws(), p_ == s_.size()); }

  private:
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\n' || s_[p_] == '\r')) ++p_;
    }
    bool lit(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s_.compare(p_, n, word) != 0) return false;
        p_ += n;
        return true;
    }
    bool string(std::string& out) {
        if (p_ >= s_.size() || s_[p_] != '"') return false;
        ++p_;
        out.clear();
        while (p_ < s_.size()) {
            char c = s_[p_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= s_.size()) return false;
            char e = s_[p_++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                if (p_ + 4 > s_.size()) return false;
                unsigned code = std::strtoul(s_.substr(p_, 4).c_str(), nullptr, 16);
                p_ += 4;
                out.push_back(code < 0x100 ? static_cast<char>(code) : '?');
                break;
            }
            default: out.push_back(e); break;
            }
        }
        return false;
    }
    bool value(Value& v, int depth) {
        if (depth > kMaxDepth) return false;
        ws();
        if (p_ >= s_.size()) return false;
        char c = s_[p_];
        if (c == '"') { v.type = Value::Type::String; return string(v.s); }
        if (c == '{') {
            v.type = Value::Type::Object;
            ++p_;
            ws();
            if (p_ < s_.size() && s_[p_] == '}') { ++p_; return true; }
            for (;;) {
                ws();
                std::string key;
                if (!string(key)) return false;
                ws();
                if (p_ >= s_.size() || s_[p_++] != ':') return false;
                Value child;
                if (!value(child, depth + 1)) return false;
                v.fields.emplace_back(std::move(key), std::move(child));
                ws();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == '}') { ++p_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++p_;
            ws();
            if (p_ < s_.size() && s_[p_] == ']') { ++p_; return true; }
            for (;;) {
                Value child;
                if (!value(child, depth + 1)) return false;
                v.items.push_back(std::move(child));
                ws();
                if (p_ >= s_.size()) return false;
                if (s_[p_] == ',') { ++p_; continue; }
                if (s_[p_] == ']') { ++p_; return true; }
                return false;
            }
        }
        if (lit("true")) { v.type = Value::Type::Bool; v.b = true; return true; }
        if (lit("false")) { v.type = Value::Type::Bool; v.b = false; return true; }
        if (lit("null")) { v.type = Value::Type::Null; return true; }
        const char* begin = s_.c_str() + p_;
        char* end = nullptr;
        double n = std::strtod(begin, &end);
        if (end == begin) return false;
        p_ += static_cast<size_t>(end - begin);
        v.type = Value::Type::Number;
        v.n = n;
        return true;
    }

    const std::string& s_;
    size_t p_ = 0;
};

bool parse(const std::string& json, Value& out) {
    if (json.size() > 256 * 1024) return false;
    Parser p(json);
    return p.parse(out) && out.type == Value::Type::Object;
}

/** Thing ids are "<Level>:<digits>" and nothing else. */
bool valid_id(const std::string& id) {
    size_t colon = id.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 64 || id.size() - colon - 1 == 0 || id.size() - colon - 1 > 20) return false;
    for (size_t i = 0; i < colon; ++i) {
        char c = id[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    for (size_t i = colon + 1; i < id.size(); ++i)
        if (id[i] < '0' || id[i] > '9') return false;
    return true;
}

std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        if (static_cast<unsigned char>(c) < 0x20) continue;
        out.push_back(c);
    }
    return out;
}

void ids(const Value* arr, std::vector<std::string>& out) {
    if (!arr || arr->type != Value::Type::Array) return;
    for (const Value& v : arr->items)
        if (v.type == Value::Type::String && valid_id(v.s)) out.push_back(v.s);
}

}  // namespace

std::string thing_id(const std::string& level, uint64_t uid) {
    return level + ":" + std::to_string(static_cast<uint32_t>(uid & 0xFFFFFFFFull));
}

std::string interact_frame(const std::string& thing, const char* action) {
    return "{\"t\":\"interact\",\"thing\":\"" + escape(thing) + "\",\"action\":\"" + escape(action ? action : "use") + "\"}";
}

std::string region_ready_frame(const std::string& region) {
    return "{\"t\":\"region_ready\",\"region\":\"" + escape(region) + "\"}";
}

bool parse_region_change(const std::string& json, RegionChange& out) {
    Value v;
    if (!parse(json, v) || v.str("t") != "enter_region") return false;
    RegionChange rc;
    rc.region = v.str("region");
    if (rc.region.empty() || rc.region.size() > 64) return false;
    double x, y, z;
    if (!v.num("x", x) || !v.num("y", y) || !v.num("z", z)) return false;
    if (std::fabs(x) > 1e6 || std::fabs(y) > 1e6 || std::fabs(z) > 1e6) return false;
    rc.x = static_cast<float>(x);
    rc.y = static_cast<float>(y);
    rc.z = static_cast<float>(z);
    rc.from = v.str("from");
    rc.via = v.str("via");
    rc.entrance = v.str("entrance");
    if (!rc.entrance.empty() && !valid_id(rc.entrance)) rc.entrance.clear();
    rc.thing = v.str("thing");
    rc.respawn = v.flag("respawn");
    if (!rc.thing.empty() && !valid_id(rc.thing)) rc.thing.clear();
    double yaw;
    if (v.num("yaw", yaw)) {
        rc.yaw = static_cast<float>(yaw);
        rc.hasYaw = true;
    }
    rc.loading = v.flag("loading");
    out = std::move(rc);
    return true;
}

ObjectFrame WorldObjects::on_frame(const std::string& json, uint32_t nowMs) {
    // Cheap reject: most frames are ticks.
    if (json.find("\"objects\"") == std::string::npos && json.find("\"object\"") == std::string::npos &&
        json.find("\"loot\"") == std::string::npos && json.find("\"error\"") == std::string::npos &&
        json.find("\"ok\"") == std::string::npos)
        return ObjectFrame::None;
    Value v;
    if (!parse(json, v)) return ObjectFrame::None;
    std::string t = v.str("t");

    if (t == "objects") {
        // The whole state of the region we just entered.
        std::string region = v.str("region");
        if (!region_.region.empty() && region != region_.region) return ObjectFrame::None;  // late frame from the old region
        for (const auto& d : doors_) changes_.push_back({d.first, d.second ? ObjectChange::What::DoorClosed : ObjectChange::What::DoorOpen});
        for (const auto& g : gone_) changes_.push_back({g, ObjectChange::What::Back});
        doors_.clear();
        gone_.clear();
        opened_.clear();
        if (const Value* doors = v.get("doors"); doors && doors->type == Value::Type::Array) {
            for (const Value& d : doors->items) {
                if (d.type != Value::Type::Array || d.items.size() != 2) continue;
                if (d.items[0].type != Value::Type::String || d.items[1].type != Value::Type::Bool || !valid_id(d.items[0].s)) continue;
                doors_[d.items[0].s] = d.items[1].b;
                changes_.push_back({d.items[0].s, d.items[1].b ? ObjectChange::What::DoorOpen : ObjectChange::What::DoorClosed});
            }
        }
        std::vector<std::string> list;
        ids(v.get("gone"), list);
        for (auto& id : list) {
            gone_.insert(id);
            changes_.push_back({id, ObjectChange::What::Gone});
        }
        list.clear();
        ids(v.get("opened"), list);
        for (auto& id : list) {
            opened_.insert(id);
            changes_.push_back({id, ObjectChange::What::Opened});
        }
        return ObjectFrame::Objects;
    }

    if (t == "object") {
        std::string id = v.str("id");
        if (!valid_id(id)) return ObjectFrame::None;
        pending_.erase(id);
        if (const Value* open = v.get("open"); open && open->type == Value::Type::Bool) {
            bool was = doors_.count(id) ? doors_[id] : !open->b;
            doors_[id] = open->b;
            if (was != open->b) changes_.push_back({id, open->b ? ObjectChange::What::DoorOpen : ObjectChange::What::DoorClosed});
        }
        if (const Value* g = v.get("gone"); g && g->type == Value::Type::Bool) {
            if (g->b && gone_.insert(id).second) changes_.push_back({id, ObjectChange::What::Gone});
            if (!g->b && gone_.erase(id)) changes_.push_back({id, ObjectChange::What::Back});
        }
        if (v.flag("opened") && opened_.insert(id).second) changes_.push_back({id, ObjectChange::What::Opened});
        return ObjectFrame::Object;
    }

    if (t == "loot") {
        Loot l;
        l.thing = v.str("thing");
        if (!valid_id(l.thing)) return ObjectFrame::None;
        pending_.erase(l.thing);
        double g = 0;
        if (v.num("gold", g) && g > 0 && g < 1e9) l.gold = static_cast<uint32_t>(g);
        if (const Value* items = v.get("items"); items && items->type == Value::Type::Array)
            for (const Value& it : items->items)
                if (it.type == Value::Type::Object) l.items.push_back({it.str("item_id"), it.str("def")});
        if (const Value* b = v.get("blocked"); b && b->type == Value::Type::Array)
            for (const Value& it : b->items)
                if (it.type == Value::Type::String) l.blocked.push_back(it.s);
        l.again = v.flag("again");
        // The hero has it now (or had it already): a chest stays open, an item is gone.
        if (opened_.insert(l.thing).second) changes_.push_back({l.thing, ObjectChange::What::Opened});
        loot_.push_back(std::move(l));
        return ObjectFrame::Loot;
    }

    if (t == "ok") {
        std::string id = v.str("thing");
        if (!valid_id(id)) return ObjectFrame::None;
        pending_.erase(id);
        acks_.push_back(id);
        return ObjectFrame::Ok;
    }

    if (t == "travel_result") {
        // Answer to a travel request: "to" is the gate id, "guild" or "back", as asked.
        std::string to = v.str("to");
        if (to.empty() || to.size() > 96) return ObjectFrame::None;
        if (v.flag("ok")) {
            acks_.push_back(to);
            return ObjectFrame::Ok;
        }
        refusals_.push_back(Refusal{to, v.str("error"), v.str("message")});
        return ObjectFrame::Refused;
    }

    if (t == "error") {
        Refusal r;
        r.thing = v.str("thing");
        if (!r.thing.empty() && !valid_id(r.thing)) r.thing.clear();
        r.code = v.str("error");
        r.message = v.str("message");
        if (!r.thing.empty()) pending_.erase(r.thing);
        // "gone" means someone else took it: hide it even if the update frame was missed.
        if (r.code == "gone" && !r.thing.empty() && gone_.insert(r.thing).second) changes_.push_back({r.thing, ObjectChange::What::Gone});
        refusals_.push_back(std::move(r));
        (void)nowMs;
        return ObjectFrame::Refused;
    }
    return ObjectFrame::None;
}

bool WorldObjects::door_open(const std::string& id, bool levelDefault) const {
    auto it = doors_.find(id);
    return it == doors_.end() ? levelDefault : it->second;
}

std::vector<ObjectChange> WorldObjects::drain_changes() {
    std::vector<ObjectChange> out;
    out.swap(changes_);
    return out;
}

std::vector<Loot> WorldObjects::drain_loot() {
    std::vector<Loot> out;
    out.swap(loot_);
    return out;
}

std::vector<Refusal> WorldObjects::drain_refusals() {
    std::vector<Refusal> out;
    out.swap(refusals_);
    return out;
}

std::vector<std::string> WorldObjects::drain_acks() {
    std::vector<std::string> out;
    out.swap(acks_);
    return out;
}

bool WorldObjects::request(const std::string& id, const char* action, uint32_t nowMs, std::string& frame) {
    if (!valid_id(id) || loading_) return false;
    auto it = pending_.find(id);
    if (it != pending_.end() && nowMs - it->second < kRequestRetryMs) return false;
    pending_[id] = nowMs;
    frame = interact_frame(id, action);
    return true;
}

void WorldObjects::begin_region(const RegionChange& rc, uint32_t nowMs) {
    clear();
    region_ = rc;
    loading_ = rc.loading;
    loadingSinceMs_ = nowMs;
}

bool WorldObjects::finish_loading(uint32_t nowMs, bool loaded, std::string& frame) {
    if (!loading_) return false;
    if (!loaded && nowMs - loadingSinceMs_ < kLoadGiveUpMs) return false;
    loading_ = false;
    frame = region_ready_frame(region_.region);
    return true;
}

void WorldObjects::clear() {
    doors_.clear();
    gone_.clear();
    opened_.clear();
    pending_.clear();
    changes_.clear();
    loot_.clear();
    refusals_.clear();
    acks_.clear();
    loading_ = false;
}

}  // namespace fo::world
