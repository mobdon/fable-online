#include "shop_stock.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "../util/inflate.h"

namespace fo::game::shop_stock {
namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kNamesHeader = 20;
constexpr size_t kRow = 12;
constexpr size_t kRowSpan = 64;               // a stock row is at most this long (server reader: ROW_SPAN)
constexpr uint32_t kArmourField = 0x909E89D5;  // CAppearanceModifierDef: the clothing's ARMOUR_HERO_* class

const char* const kBannedItems[] = {"OBJECT_RESURRECTION_PHIAL"};
const char* const kBannedArmour[] = {"ARMOUR_HERO_PLATE_ARMOUR", "ARMOUR_HERO_PLATINUM_ARMOUR"};

uint32_t rd32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// Byte search for a little-endian u32, like the server's Buffer.indexOf.
long find32(const uint8_t* p, size_t n, uint32_t v, size_t from = 0) {
    uint8_t k[4];
    std::memcpy(k, &v, 4);
    for (size_t i = from; i + 4 <= n; ++i)
        if (p[i] == k[0] && p[i + 1] == k[1] && p[i + 2] == k[2] && p[i + 3] == k[3]) return static_cast<long>(i);
    return -1;
}

struct EntryRef {
    size_t chunk = 0;
    size_t begin = 0, end = 0;  // inside that chunk's inflated bytes
};

enum class Kind : uint8_t { Type, Def, Component };

}  // namespace

uint32_t fable_hash(const char* s) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
            table[i] = c;
        }
        built = true;
    }
    uint32_t c = 0;
    for (; *s; ++s) c = table[(c ^ static_cast<uint8_t>(*s)) & 0xFF] ^ (c >> 8);
    return c;
}

Report filter(const std::vector<uint8_t>& g, const std::vector<uint8_t>& names, std::vector<uint8_t>& out) {
    Report r;
    out.clear();
    auto fail = [&](const char* why) { r.error = why; out.clear(); return r; };

    if (names.size() < kNamesHeader) return fail("names.bin is too short");
    auto name_at = [&](uint32_t off) -> std::string {
        size_t s = kNamesHeader + off;
        if (s < kNamesHeader || s >= names.size()) return {};
        const char* p = reinterpret_cast<const char*>(&names[s]);
        return std::string(p, strnlen(p, names.size() - s));
    };

    if (g.size() < 13) return fail("game.bin is too short");
    if (g[0] != 0) return fail("game.bin does not use names.bin");
    if (rd32(&g[5]) != rd32(&names[4])) return fail("game.bin and names.bin are from different builds");
    const uint32_t count = rd32(&g[9]);
    size_t o = 13;
    if (count == 0 || count > 2000000 || o + size_t(count) * kRow + 4 > g.size()) return fail("bad entry count");

    std::vector<Kind> kind(count);
    std::vector<std::string> label(count);  // def name, or component type name
    bool seen_def = false;
    for (uint32_t i = 0; i < count; ++i, o += kRow) {
        uint32_t a = rd32(&g[o]), b = rd32(&g[o + 4]), c = rd32(&g[o + 8]);
        if (b == kNone) { kind[i] = Kind::Component; label[i] = name_at(a); }
        else if (c == 0 && !seen_def) { kind[i] = Kind::Type; }
        else { kind[i] = Kind::Def; label[i] = name_at(b); seen_def = true; }
    }

    const uint32_t nchunks = rd32(&g[o]);
    o += 4;
    if (nchunks < 2 || o + size_t(nchunks) * 8 + 4 > g.size()) return fail("bad chunk table");
    const size_t table_pos = o;
    std::vector<uint32_t> first(nchunks), off(nchunks);
    for (uint32_t k = 0; k < nchunks; ++k, o += 8) { first[k] = rd32(&g[o]); off[k] = rd32(&g[o + 4]); }
    const size_t size_pos = o;
    const uint32_t size_field = rd32(&g[o]);
    o += 4;
    const size_t base = o;
    if (first[0] != 0 || first[nchunks - 1] != count) return fail("chunk table doesn't cover the entries");

    // Inflate every chunk and note where each entry sits.
    std::vector<std::vector<uint8_t>> chunk(nchunks - 1);
    std::vector<EntryRef> entry(count);
    size_t max_packed = 0, max_plain = 0, total_plain = 0;
    for (uint32_t k = 0; k + 1 < nchunks; ++k) {
        if (off[k + 1] < off[k] || first[k + 1] < first[k] || base + off[k + 1] > g.size()) return fail("bad chunk offsets");
        if (!util::zlib_inflate(&g[base + off[k]], off[k + 1] - off[k], chunk[k])) return fail("a chunk didn't inflate");
        max_packed = std::max<size_t>(max_packed, off[k + 1] - off[k]);
        max_plain = std::max(max_plain, chunk[k].size());
        total_plain += chunk[k].size();
        const auto& u = chunk[k];
        uint32_t n = first[k + 1] - first[k];
        if (size_t(n) * 2 > u.size()) return fail("chunk too short for its entry offsets");
        for (uint32_t j = 0; j < n; ++j) {
            size_t s = rd16(&u[j * 2]);
            size_t e = j + 1 < n ? rd16(&u[(j + 1) * 2]) : u.size();
            if (s > e || e > u.size()) return fail("bad entry offsets in a chunk");
            entry[first[k] + j] = {k, s, e};
        }
    }
    auto bytes = [&](uint32_t i) { return chunk[entry[i].chunk].data() + entry[i].begin; };
    auto length = [&](uint32_t i) { return entry[i].end - entry[i].begin; };

    // Which item defs are banned.
    std::set<uint32_t> banned_armour_idx;
    for (uint32_t i = 0; i < count; ++i)
        if (kind[i] == Kind::Def)
            for (const char* a : kBannedArmour) if (label[i] == a) banned_armour_idx.insert(i);
    const uint32_t look_type = fable_hash("CAppearanceModifierDef");
    std::set<uint32_t> banned;
    for (uint32_t i = 0; i < count; ++i) {
        if (kind[i] != Kind::Def || label[i].compare(0, 7, "OBJECT_") != 0) continue;
        for (const char* b : kBannedItems) if (label[i] == b) banned.insert(i);
        const uint8_t* d = bytes(i);
        size_t n = length(i);
        if (n < 5) continue;
        unsigned comps = rd16(d + 3);
        for (unsigned j = 0; j < comps && 5 + (j + 1) * kRow <= n; ++j) {
            const uint8_t* row = d + 5 + j * kRow;
            uint32_t ci = rd32(row + 4);
            if (rd32(row) != look_type || ci >= count) continue;
            long at = find32(bytes(ci), length(ci), kArmourField);
            if (at < 0 || size_t(at) + 8 > length(ci)) continue;
            if (banned_armour_idx.count(rd32(bytes(ci) + at + 4))) banned.insert(i);
        }
    }
    r.banned_defs = static_cast<unsigned>(banned.size());
    r.banned.assign(banned.begin(), banned.end());

    // Empty the banned rows of every shop.
    const uint32_t type_key = fable_hash("Type"), qty_key = fable_hash("Quantity"), max_key = fable_hash("MaxStock");
    std::vector<bool> dirty(nchunks - 1, false);
    std::set<std::string> cleared;
    for (uint32_t i = 0; i < count; ++i) {
        if (kind[i] != Kind::Component || label[i] != "CShopDef") continue;
        ++r.shops;
        uint8_t* d = chunk[entry[i].chunk].data() + entry[i].begin;
        size_t n = length(i);
        long at = find32(d, n, type_key);
        while (at >= 0 && size_t(at) + 8 <= n) {
            long next = find32(d, n, type_key, at + 4);
            size_t end = next < 0 ? std::min(n, size_t(at) + kRowSpan) : size_t(next);
            uint32_t item = rd32(d + at + 4);
            if (banned.count(item)) {
                bool changed = false;
                for (uint32_t key : {qty_key, max_key}) {
                    long f = find32(d + at, end - at, key);
                    if (f < 0 || size_t(at + f) + 8 > end) continue;
                    uint8_t* v = d + at + f + 4;
                    if (rd32(v) != 0) { wr32(v, 0); changed = true; }  // 0.0f
                }
                if (changed) {
                    ++r.rows_cleared;
                    dirty[entry[i].chunk] = true;
                    cleared.insert(label[item]);
                }
            }
            at = next;
        }
    }
    r.cleared.assign(cleared.begin(), cleared.end());

    if (!r.rows_cleared) {
        out = g;
        r.ok = true;
        return r;
    }

    // Rebuild: changed chunks as stored zlib, the rest byte for byte.
    std::vector<std::vector<uint8_t>> packed(nchunks - 1);
    size_t data_len = 0, new_max_packed = 0;
    for (uint32_t k = 0; k + 1 < nchunks; ++k) {
        if (dirty[k]) { packed[k] = util::zlib_store(chunk[k].data(), chunk[k].size()); ++r.chunks_rewritten; }
        else packed[k].assign(g.begin() + base + off[k], g.begin() + base + off[k + 1]);
        data_len += packed[k].size();
        new_max_packed = std::max(new_max_packed, packed[k].size());
    }

    // The u32 before the data: work out what it measures from the original file, so it can be
    // kept true. If it matches nothing we know, leave the game's file alone.
    const size_t old_data_len = off[nchunks - 1] - off[0];
    const size_t tail = g.size() - (base + off[nchunks - 1]);
    uint32_t new_size_field;
    if (size_field == g.size() - base) new_size_field = static_cast<uint32_t>(off[0] + data_len + tail);
    else if (size_field == old_data_len) new_size_field = static_cast<uint32_t>(data_len);
    else if (size_field == max_packed) new_size_field = static_cast<uint32_t>(new_max_packed);
    else if (size_field == total_plain || size_field == max_plain) new_size_field = size_field;
    else return fail("unknown size field before the chunk data");

    out.reserve(g.size() + data_len - old_data_len);
    out.assign(g.begin(), g.begin() + base + off[0]);
    for (const auto& p : packed) out.insert(out.end(), p.begin(), p.end());
    out.insert(out.end(), g.begin() + base + off[nchunks - 1], g.end());
    uint32_t pos = off[0];
    for (uint32_t k = 0; k < nchunks; ++k) {
        wr32(&out[table_pos + k * 8 + 4], pos);
        if (k + 1 < nchunks) pos += static_cast<uint32_t>(packed[k].size());
    }
    wr32(&out[size_pos], new_size_field);
    r.ok = true;
    return r;
}

}  // namespace fo::game::shop_stock
