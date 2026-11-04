#include "snapshot.h"

#include <cmath>
#include <cstdlib>

namespace fo::world {
namespace {

// A small reader over one JSON text. It never allocates a tree: the frames are flat
// objects and one array of flat objects, so a scanner is enough and cannot run away on a
// deeply nested frame.
class Reader {
  public:
    explicit Reader(const std::string& s) : s_(&s) {}

    void skip_ws() {
        while (p_ < s_->size() && ((*s_)[p_] == ' ' || (*s_)[p_] == '\t' || (*s_)[p_] == '\n' || (*s_)[p_] == '\r')) ++p_;
    }
    bool eof() const { return p_ >= s_->size(); }
    char peek() const { return p_ < s_->size() ? (*s_)[p_] : '\0'; }
    bool eat(char c) {
        skip_ws();
        if (peek() != c) return false;
        ++p_;
        return true;
    }
    void expect_any() {  // skip one value, whatever it is
        skip_ws();
        char c = peek();
        if (c == '"') { std::string ignored; read_string(ignored); return; }
        if (c == '{' || c == '[') { skip_container(); return; }
        while (p_ < s_->size() && (*s_)[p_] != ',' && (*s_)[p_] != '}' && (*s_)[p_] != ']') ++p_;
    }

    bool read_string(std::string& out) {
        skip_ws();
        if (peek() != '"') return false;
        ++p_;
        out.clear();
        while (p_ < s_->size()) {
            char c = (*s_)[p_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p_ >= s_->size()) return false;
            char e = (*s_)[p_++];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                if (p_ + 4 > s_->size()) return false;
                unsigned code = std::strtoul(s_->substr(p_, 4).c_str(), nullptr, 16);
                p_ += 4;
                // The mod only renders Latin-1 text (Fable's own fonts); anything above
                // that becomes '?' rather than a broken multi-byte sequence.
                out.push_back(code < 0x100 ? static_cast<char>(code) : '?');
                break;
            }
            default: out.push_back(e); break;
            }
        }
        return false;
    }

    bool read_number(double& out) {
        skip_ws();
        const char* begin = s_->c_str() + p_;
        char* end = nullptr;
        double v = std::strtod(begin, &end);
        if (end == begin) return false;
        p_ += static_cast<size_t>(end - begin);
        out = v;
        return true;
    }

    /** Reads `"key":` and leaves the position on the value. */
    bool read_key(std::string& key) {
        if (!read_string(key)) return false;
        return eat(':');
    }

    void skip_container() {
        skip_ws();
        char open = peek();
        if (open != '{' && open != '[') return;
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        while (p_ < s_->size()) {
            char c = (*s_)[p_++];
            if (in_string) {
                if (c == '\\') ++p_;
                else if (c == '"') in_string = false;
                continue;
            }
            if (c == '"') in_string = true;
            else if (c == open) ++depth;
            else if (c == close && --depth == 0) return;
        }
    }

  private:
    const std::string* s_;
    size_t p_ = 0;
};

bool sane(float v) { return std::isfinite(v) && std::fabs(v) < 1.0e6f; }

/** Reads the fields of one entity object. Position is left at the closing brace. */
bool read_entity(Reader& r, EntityState& e) {
    if (!r.eat('{')) return false;
    if (r.eat('}')) return false;  // an empty entity is not usable
    bool have_pos = false;
    for (;;) {
        std::string key;
        if (!r.read_key(key)) return false;
        double n = 0;
        if (key == "id") {
            if (!r.read_number(n)) return false;
            e.id = static_cast<uint32_t>(n);
        } else if (key == "x" || key == "y" || key == "z" || key == "yaw" || key == "health" || key == "max_health" || key == "renown" ||
                   key == "anim" || key == "kind_id") {
            if (!r.read_number(n)) return false;
            if (key == "x") { e.x = static_cast<float>(n); have_pos = true; }
            else if (key == "y") e.y = static_cast<float>(n);
            else if (key == "z") e.z = static_cast<float>(n);
            else if (key == "yaw") e.yaw = static_cast<float>(n);
            else if (key == "health") e.health = static_cast<float>(n);
            else if (key == "max_health") e.maxHealth = static_cast<float>(n);
            else if (key == "renown") e.renown = static_cast<uint32_t>(n);
            else if (key == "anim") e.anim = n >= 0 && n <= 10 ? static_cast<Anim>(static_cast<uint8_t>(n)) : Anim::Idle;
        } else if (key == "def") {
            if (!r.read_string(e.def)) return false;
        } else if (key == "name") {
            if (!r.read_string(e.name)) return false;
        } else if (key == "kind") {
            std::string k;
            if (!r.read_string(k)) return false;
            e.kind = k == "hero" ? Kind::Hero : Kind::Creature;
        } else {
            r.expect_any();
        }
        if (r.eat(',')) continue;
        if (r.eat('}')) break;
        return false;
    }
    // A creature is anything the server gave a def name; heroes have no def.
    if (e.kind == Kind::Creature && e.def.empty()) e.kind = Kind::Hero;
    return have_pos && e.id != 0 && sane(e.x) && sane(e.y) && sane(e.z) && sane(e.yaw);
}

/** Finds a top-level key and leaves the reader on its value. */
bool seek_key(Reader& r, const char* wanted, std::string* found_type = nullptr) {
    if (!r.eat('{')) return false;
    for (;;) {
        std::string key;
        if (!r.read_key(key)) return false;
        if (key == wanted) return true;
        if (found_type && key == "t") r.read_string(*found_type);
        else r.expect_any();
        if (r.eat(',')) continue;
        return false;
    }
}

std::string frame_tag(const std::string& json) {
    Reader r(json);
    std::string tag;
    if (!r.eat('{')) return tag;
    for (;;) {
        std::string key;
        if (!r.read_key(key)) return tag;
        if (key == "t") { r.read_string(tag); return tag; }
        r.expect_any();
        if (!r.eat(',')) return tag;
    }
}

}  // namespace

FrameType frame_type(const std::string& json) {
    std::string tag = frame_tag(json);
    if (tag == "tick") return FrameType::Tick;
    if (tag == "welcome") return FrameType::Welcome;
    if (tag == "enter_region") return FrameType::EnterRegion;
    if (tag == "error") return FrameType::Error;
    if (tag == "pos_correct") return FrameType::PosCorrect;
    if (tag == "pong") return FrameType::Pong;
    return FrameType::Unknown;
}

bool parse_tick(const std::string& json, Tick& out) {
    if (frame_type(json) != FrameType::Tick) return false;
    out = Tick{};
    {
        Reader r(json);
        double n = 0;
        if (seek_key(r, "tick") && r.read_number(n)) out.tick = static_cast<uint32_t>(n);
    }
    {
        Reader r(json);
        double n = 0;
        if (seek_key(r, "last_input_seq") && r.read_number(n)) out.lastInputSeq = static_cast<uint32_t>(n);
    }
    Reader r(json);
    if (!seek_key(r, "entities")) return false;
    if (!r.eat('[')) return false;
    if (r.eat(']')) return true;  // an empty region is a valid frame
    for (;;) {
        EntityState e;
        Reader before = r;
        if (read_entity(r, e)) out.entities.push_back(e);
        else {
            // One malformed entity must not cost us the rest of the frame.
            r = before;
            r.skip_container();
        }
        if (r.eat(',')) continue;
        break;
    }
    return true;
}

bool parse_welcome(const std::string& json, Welcome& out) {
    if (frame_type(json) != FrameType::Welcome) return false;
    out = Welcome{};
    double n = 0;
    {
        Reader r(json);
        if (!seek_key(r, "player_id") || !r.read_number(n)) return false;
        out.playerId = static_cast<uint32_t>(n);
    }
    {
        Reader r(json);
        if (seek_key(r, "name")) r.read_string(out.name);
    }
    {
        Reader r(json);
        if (seek_key(r, "tick_rate") && r.read_number(n) && n > 0 && n <= 120) out.tickRate = static_cast<uint32_t>(n);
    }
    {
        Reader r(json);
        if (seek_key(r, "resume")) r.read_string(out.resume);
        if (out.resume.size() > 256) out.resume.clear();
    }
    {
        Reader r(json);
        std::string mode;
        if (seek_key(r, "client_move") && r.read_string(mode) && (mode == "off" || mode == "checked" || mode == "trusted"))
            out.clientMove = mode;
    }
    return out.playerId != 0;
}

bool parse_pos_correct(const std::string& json, PosCorrect& out) {
    if (frame_type(json) != FrameType::PosCorrect) return false;
    out = PosCorrect{};
    const char* keys[3] = {"x", "y", "z"};
    float* dst[3] = {&out.x, &out.y, &out.z};
    for (int i = 0; i < 3; ++i) {
        Reader r(json);
        double n = 0;
        if (!seek_key(r, keys[i]) || !r.read_number(n) || !std::isfinite(n) || std::fabs(n) > 1e6) return false;
        *dst[i] = static_cast<float>(n);
    }
    Reader r(json);
    if (seek_key(r, "reason")) r.read_string(out.reason);
    Reader ra(json);
    double area = 0;
    if (seek_key(ra, "area") && ra.read_number(area) && area >= 0 && area <= 4294967295.0) out.area = static_cast<uint32_t>(area);
    return true;
}

bool parse_pong(const std::string& json, double& n) {
    if (frame_type(json) != FrameType::Pong) return false;
    Reader r(json);
    return seek_key(r, "n") && r.read_number(n);
}

bool parse_enter_region(const std::string& json, std::string& region, float& x, float& y, float& z) {
    if (frame_type(json) != FrameType::EnterRegion) return false;
    {
        Reader r(json);
        if (seek_key(r, "region")) r.read_string(region);
    }
    double n = 0;
    const char* keys[3] = {"x", "y", "z"};
    float* outs[3] = {&x, &y, &z};
    for (int i = 0; i < 3; ++i) {
        Reader r(json);
        if (!seek_key(r, keys[i]) || !r.read_number(n)) return false;
        *outs[i] = static_cast<float>(n);
        if (!sane(*outs[i])) return false;
    }
    return true;
}

bool parse_error(const std::string& json, std::string& code, std::string& message) {
    if (frame_type(json) != FrameType::Error) return false;
    {
        Reader r(json);
        if (seek_key(r, "error")) r.read_string(code);
    }
    Reader r(json);
    if (seek_key(r, "message")) r.read_string(message);
    return !code.empty();
}

}  // namespace fo::world
