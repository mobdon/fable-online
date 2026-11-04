#include "nametags.h"
#include <algorithm>
#include <cmath>

namespace fo::ui {

uint32_t renown_color(double r) {
    // Pewter, bronze, silver, gold, then the chrome blue of Fable's logo for legends.
    if (r >= 50000) return 0xFF9AA3F4;
    if (r >= 20000) return 0xFFE8C15A;
    if (r >= 5000) return 0xFFD9DEE6;
    if (r >= 1000) return 0xFFC8955A;
    return 0xFFC9C0A8;
}

std::string format_renown(double r) {
    long long v = r < 0 ? 0 : static_cast<long long>(r + 0.5);
    std::string d = std::to_string(v), o;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) o += ',';
        o += d[i];
    }
    return o + " renown";
}

bool view_from_camera(const float pos[3], const float fwd[3], const float up[3], const float* ref, float m[16]) {
    auto norm = [](float v[3]) {
        float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (!(l > 1e-6f) || !std::isfinite(l)) return false;
        v[0] /= l; v[1] /= l; v[2] /= l;
        return true;
    };
    float z[3] = {fwd[0], fwd[1], fwd[2]};
    if (!norm(z)) return false;
    float x[3] = {up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2], up[0] * z[1] - up[1] * z[0]};
    if (!norm(x)) return false;
    float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    // Point each axis the way the game's own view matrix does (its columns): that takes its
    // handedness and any mirroring, whatever convention the engine uses.
    float* axes[3] = {x, y, z};
    if (ref)
        for (int c = 0; c < 3; ++c) {
            const float d = ref[c] * axes[c][0] + ref[4 + c] * axes[c][1] + ref[8 + c] * axes[c][2];
            if (d < 0) for (int k = 0; k < 3; ++k) axes[c][k] = -axes[c][k];
        }
    auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    const float r[16] = {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, pos), -dot(y, pos), -dot(z, pos), 1};
    for (int i = 0; i < 16; ++i) m[i] = r[i];
    return true;
}

namespace {
void read_tag(const json::Value& v, Tag& t) {
    t.id = static_cast<uint32_t>(v.num("id"));
    t.name = v.str("name");
    t.title = v.str("title");
    t.guild = v.str("guild");
    t.renown = v.num("renown");
    t.pvp = v.boolean("pvp");
    t.staff = v.str("staff");
    if (t.staff != "DEV" && t.staff != "GM") t.staff.clear();
    t.x = static_cast<float>(v.num("x"));
    t.y = static_cast<float>(v.num("y"));
    t.z = static_cast<float>(v.num("z"));
}

uint32_t with_alpha(uint32_t argb, float a) {
    uint32_t base = (argb >> 24) & 0xFF;
    uint32_t na = static_cast<uint32_t>(std::lround(base * std::clamp(a, 0.0f, 1.0f)));
    return (na << 24) | (argb & 0x00FFFFFF);
}
}  // namespace

void TagStore::apply(const json::Value& m) {
    uint32_t region = static_cast<uint32_t>(m.num("region"));
    if (region != region_) { tags_.clear(); region_ = region; }
    for (const auto& v : m["add"].arr) {
        Tag t;
        read_tag(v, t);
        t.px = t.x; t.py = t.y; t.pz = t.z;  // new tags appear in place, no sliding in
        tags_[t.id] = t;
    }
    for (const auto& v : m["upd"].arr) {
        Tag t;
        read_tag(v, t);
        auto it = tags_.find(t.id);
        if (it == tags_.end()) { t.px = t.x; t.py = t.y; t.pz = t.z; tags_[t.id] = t; continue; }
        t.px = it->second.px; t.py = it->second.py; t.pz = it->second.pz;
        it->second = t;
    }
    for (const auto& v : m["move"].arr) {
        auto it = tags_.find(static_cast<uint32_t>(v.num("id")));
        if (it == tags_.end()) continue;
        it->second.x = static_cast<float>(v.num("x"));
        it->second.y = static_cast<float>(v.num("y"));
        it->second.z = static_cast<float>(v.num("z"));
    }
    for (const auto& v : m["del"].arr) tags_.erase(static_cast<uint32_t>(v.n));
}

void TagStore::set_self(bool on, const std::string& name, double renown, float x, float y, float z,
                        const std::string& staff) {
    self_on_ = on;
    self_.staff = staff == "DEV" || staff == "GM" ? staff : "";
    self_.id = 0;
    self_.name = name;
    self_.renown = renown;
    self_.title = "(you)";
    self_.x = self_.px = x; self_.y = self_.py = y; self_.z = self_.pz = z;
}

void TagStore::step(float dt) {
    // Server updates come 5 times a second; ease so tags glide instead of hopping.
    float k = 1.0f - std::exp(-dt * 12.0f);
    for (auto& [id, t] : tags_) {
        auto l = live_.find(id);
        if (l != live_.end()) { t.px = l->second[0]; t.py = l->second[1]; t.pz = l->second[2]; continue; }
        float dx = t.x - t.px, dy = t.y - t.py, dz = t.z - t.pz;
        if (dx * dx + dy * dy + dz * dz > 100.0f) { t.px = t.x; t.py = t.y; t.pz = t.z; continue; }  // teleport
        t.px += dx * k; t.py += dy * k; t.pz += dz * k;
    }
}

std::vector<Label> TagStore::layout(const float m[16], float w, float h, const TagSettings& s) const {
    std::vector<Label> out;
    auto place = [&](const Tag& t) {
        float p[3] = {t.px, t.py, t.pz};
        p[std::clamp(s.up_axis, 0, 2)] += s.head_height;
        float cx = p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12];
        float cy = p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13];
        float cw = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        if (cw < 0.1f) return;                                  // behind the camera
        float nx = cx / cw, ny = cy / cw;
        if (nx < -1.1f || nx > 1.1f || ny < -1.1f || ny > 1.1f) return;  // off screen
        float dist = cw;                                         // perspective w = view depth
        float a = 1.0f;
        if (dist > s.hidden_from) return;
        if (dist > s.full_until) a = 1.0f - (dist - s.full_until) / (s.hidden_from - s.full_until);
        Label l;
        l.x = (nx + 1.0f) * 0.5f * w;
        l.y = (1.0f - ny) * 0.5f * h;
        l.name_line = t.guild.empty() ? t.name : t.name + " <" + t.guild + ">";
        l.info_line = (t.title.empty() ? "" : t.title + " - ") + format_renown(t.renown);
        l.name_color = with_alpha(t.pvp ? 0xFFE0503C : 0xFFFFF6DC, a);
        l.info_color = with_alpha(renown_color(t.renown), a);
        l.depth = dist;
        l.pvp = t.pvp;
        l.self = &t == &self_;
        l.badge = t.staff;
        if (l.self) l.name_color = with_alpha(0xFFFFF4C8, a);   // pale gold on the gold plate
        out.push_back(std::move(l));
    };
    for (const auto& [id, t] : tags_) place(t);
    if (self_on_) place(self_);

    // Nearest first; a farther tag that would sit on a nearer one is nudged up.
    std::sort(out.begin(), out.end(), [](const Label& a, const Label& b) { return a.depth < b.depth; });
    const float lh = s.line_height, half_w = 70.0f;
    for (size_t i = 1; i < out.size(); ++i) {
        for (int tries = 0; tries < 4; ++tries) {
            bool hit = false;
            for (size_t j = 0; j < i; ++j) {
                if (std::fabs(out[i].x - out[j].x) < half_w * 2 && std::fabs(out[i].y - out[j].y) < lh * 2) {
                    out[i].y = out[j].y - lh * 2;
                    hit = true;
                }
            }
            if (!hit) break;
        }
    }
    // Draw back to front so near tags end up on top.
    std::reverse(out.begin(), out.end());
    return out;
}

}  // namespace fo::ui
