#include "defs_edit.h"

#include <algorithm>
#include <cstring>

#include "../util/inflate.h"

namespace fo::game::defs {
namespace {
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kNamesHeader = 20;
constexpr size_t kRow = 12;
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
void wr16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
}  // namespace

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

long find32(const uint8_t* p, size_t n, uint32_t v, size_t from) {
    uint8_t k[4];
    std::memcpy(k, &v, 4);
    for (size_t i = from; i + 4 <= n; ++i)
        if (p[i] == k[0] && p[i + 1] == k[1] && p[i + 2] == k[2] && p[i + 3] == k[3]) return static_cast<long>(i);
    return -1;
}

bool Bin::load(const std::vector<uint8_t>& g, const std::vector<uint8_t>& names) {
    auto fail = [&](const char* why) { error = why; entries_.clear(); return false; };
    file_ = g;
    if (names.size() < kNamesHeader) return fail("names.bin is too short");
    if (g.size() < 13) return fail("game.bin is too short");
    if (g[0] != 0) return fail("game.bin does not use names.bin");
    if (rd32(&g[5]) != rd32(&names[4])) return fail("game.bin and names.bin are from different builds");
    const uint32_t count = rd32(&g[9]);
    size_t o = 13;
    if (count == 0 || count > 2000000 || o + size_t(count) * kRow + 4 > g.size()) return fail("bad entry count");
    auto name_at = [&](uint32_t off) -> std::string {
        size_t s = kNamesHeader + off;
        if (s < kNamesHeader || s >= names.size()) return {};
        const char* p = reinterpret_cast<const char*>(&names[s]);
        return std::string(p, strnlen(p, names.size() - s));
    };
    def_names_.assign(count, {});
    bool seen_def = false;
    for (uint32_t i = 0; i < count; ++i, o += kRow) {
        uint32_t b = rd32(&g[o + 4]), c = rd32(&g[o + 8]);
        if (b == kNone) continue;
        if (c == 0 && !seen_def) continue;
        def_names_[i] = name_at(b);
        seen_def = true;
    }
    const uint32_t nchunks = rd32(&g[o]);
    o += 4;
    if (nchunks < 2 || o + size_t(nchunks) * 8 + 4 > g.size()) return fail("bad chunk table");
    table_pos_ = o;
    first_.resize(nchunks);
    off_.resize(nchunks);
    for (uint32_t k = 0; k < nchunks; ++k, o += 8) { first_[k] = rd32(&g[o]); off_[k] = rd32(&g[o + 4]); }
    size_pos_ = o;
    size_field_ = rd32(&g[o]);
    base_ = o + 4;
    if (first_[0] != 0 || first_[nchunks - 1] != count) return fail("chunk table doesn't cover the entries");

    entries_.assign(count, {});
    chunk_of_.assign(count, 0);
    dirty_.assign(nchunks - 1, false);
    max_packed_ = max_plain_ = total_plain_ = 0;
    for (uint32_t k = 0; k + 1 < nchunks; ++k) {
        if (off_[k + 1] < off_[k] || first_[k + 1] < first_[k] || base_ + off_[k + 1] > g.size()) return fail("bad chunk offsets");
        std::vector<uint8_t> u;
        if (!util::zlib_inflate(&g[base_ + off_[k]], off_[k + 1] - off_[k], u)) return fail("a chunk didn't inflate");
        max_packed_ = std::max<size_t>(max_packed_, off_[k + 1] - off_[k]);
        max_plain_ = std::max(max_plain_, u.size());
        total_plain_ += u.size();
        const uint32_t n = first_[k + 1] - first_[k];
        if (size_t(n) * 2 > u.size()) return fail("chunk too short for its entry offsets");
        for (uint32_t j = 0; j < n; ++j) {
            size_t s = rd16(&u[j * 2]);
            size_t e = j + 1 < n ? rd16(&u[(j + 1) * 2]) : u.size();
            if (s > e || e > u.size()) return fail("bad entry offsets in a chunk");
            entries_[first_[k] + j].assign(u.begin() + s, u.begin() + e);
            chunk_of_[first_[k] + j] = k;
        }
    }
    return true;
}

int Bin::find_def(const std::string& name) const {
    for (size_t i = 0; i < def_names_.size(); ++i)
        if (def_names_[i] == name) return static_cast<int>(i);
    return -1;
}

std::vector<uint8_t>& Bin::edit(uint32_t i) {
    dirty_[chunk_of_[i]] = true;
    return entries_[i];
}

bool Bin::save(std::vector<uint8_t>& out) {
    const uint32_t nchunks = static_cast<uint32_t>(first_.size());
    if (std::none_of(dirty_.begin(), dirty_.end(), [](bool d) { return d; })) { out = file_; return true; }

    std::vector<std::vector<uint8_t>> packed(nchunks - 1);
    size_t data_len = 0, new_max_packed = 0, new_max_plain = 0;
    for (uint32_t k = 0; k + 1 < nchunks; ++k) {
        if (dirty_[k]) {
            const uint32_t n = first_[k + 1] - first_[k];
            std::vector<uint8_t> u(size_t(n) * 2);
            for (uint32_t j = 0; j < n; ++j) {
                if (u.size() > 0xFFFF) { error = "a rewritten chunk is over 64 KB"; return false; }
                wr16(&u[j * 2], static_cast<uint16_t>(u.size()));
                const auto& e = entries_[first_[k] + j];
                u.insert(u.end(), e.begin(), e.end());
            }
            new_max_plain = std::max(new_max_plain, u.size());
            packed[k] = util::zlib_store(u.data(), u.size());
        } else {
            packed[k].assign(file_.begin() + base_ + off_[k], file_.begin() + base_ + off_[k + 1]);
        }
        data_len += packed[k].size();
        new_max_packed = std::max(new_max_packed, packed[k].size());
    }

    // The u32 before the data: keep it true to whatever it measured in the original file.
    const size_t old_data_len = off_[nchunks - 1] - off_[0];
    const size_t tail = file_.size() - (base_ + off_[nchunks - 1]);
    uint32_t new_size_field;
    if (size_field_ == file_.size() - base_) new_size_field = static_cast<uint32_t>(off_[0] + data_len + tail);
    else if (size_field_ == old_data_len) new_size_field = static_cast<uint32_t>(data_len);
    else if (size_field_ == max_packed_) new_size_field = static_cast<uint32_t>(new_max_packed);
    else if (size_field_ == max_plain_) new_size_field = static_cast<uint32_t>(std::max(max_plain_, new_max_plain));
    else { error = "unknown size field before the chunk data"; return false; }

    out.clear();
    out.reserve(file_.size() + data_len - old_data_len);
    out.assign(file_.begin(), file_.begin() + base_ + off_[0]);
    for (const auto& p : packed) out.insert(out.end(), p.begin(), p.end());
    out.insert(out.end(), file_.begin() + base_ + off_[nchunks - 1], file_.end());
    uint32_t pos = off_[0];
    for (uint32_t k = 0; k < nchunks; ++k) {
        wr32(&out[table_pos_ + k * 8 + 4], pos);
        if (k + 1 < nchunks) pos += static_cast<uint32_t>(packed[k].size());
    }
    wr32(&out[size_pos_], new_size_field);
    return true;
}

}  // namespace fo::game::defs
