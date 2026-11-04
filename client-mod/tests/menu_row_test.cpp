// The in-game menu row patch on a small game.bin built here with the same layout as the
// retail file (tools/re-notes/07-game-bin.md): a list def whose Children name the rows, rows
// with PositionY per state and a message field, and a label with a UTF-16 text field.
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/game/defs_edit.h"
#include "../src/game/menu_row.h"
#include "../src/util/inflate.h"

using fo::game::defs::Bin;
using Bytes = std::vector<uint8_t>;

namespace {
void put32(Bytes& b, uint32_t v) { for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(v >> (8 * i))); }
void putf(Bytes& b, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(b, u); }
void put16(Bytes& b, uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); }
void putw(Bytes& b, const std::string& s) { for (char c : s) { b.push_back(static_cast<uint8_t>(c)); b.push_back(0); } put16(b, 0); }

constexpr uint32_t kChildren = 0x3DC30C85, kPositionY = 0x69DCBAA7, kMessage = 0xB40B9CDE, kText = 0xE215EF13, kType = 0x0DA8270B;

Bytes ui_head(uint32_t type, const std::vector<uint32_t>& children) {
    Bytes e = {1, 0, 1};
    put16(e, 0);
    put32(e, kType); put32(e, type);
    put32(e, kChildren); put32(e, static_cast<uint32_t>(children.size()));
    for (uint32_t c : children) put32(e, c);
    return e;
}
Bytes row(float y, uint32_t msg) {
    Bytes e = ui_head(7, {});
    for (int s = 0; s < 6; ++s) { put32(e, kPositionY); putf(e, s == 2 ? 0.0f : y); put32(e, 0x11111111); put32(e, 7); }
    put32(e, kMessage); put32(e, msg);
    return e;
}
Bytes label(const std::string& s) {
    Bytes e = ui_head(6, {});
    put32(e, kText); putw(e, s);
    put32(e, 0x51E278F0); put32(e, 0xE0);
    return e;
}

struct Fixture { Bytes game, names; };

// Entries: 0 the UI type, then defs 1..7.
Fixture make() {
    const char* defs[] = {"PC_TOP_LEVEL_LIST", "PC_MENU_ENTRY_OPTIONS", "PC_MENU_ENTRY_RESUME", "PC_MENU_ENTRY_MSNCHAT",
                          "UI_MSNCHAT_TEXT", "PC_TOP_LEVEL_ORNAMENT_BOTTOM", "PC_MENU_ENTRY_SCOREBOARD"};
    Fixture f;
    Bytes body;
    std::vector<uint32_t> offs;
    auto add_name = [&](const char* s) {
        put32(body, 0x12345678);
        offs.push_back(static_cast<uint32_t>(body.size()));
        body.insert(body.end(), s, s + std::strlen(s) + 1);
    };
    add_name("UI");
    for (const char* d : defs) add_name(d);
    put32(f.names, static_cast<uint32_t>(body.size())); put32(f.names, 0xA8E36C34); put32(f.names, 8); put32(f.names, 0); put32(f.names, 0);
    f.names.insert(f.names.end(), body.begin(), body.end());

    std::vector<Bytes> e(8);
    e[0] = {1, 0, 1};
    e[1] = ui_head(8, {2, 3, 6});            // Options, Resume, ornament
    e[2] = row(125, 239);
    e[3] = row(150, 0);
    e[4] = row(125, 0);                       // MSN chat: unlisted
    e[5] = label("Msn Chat");
    e[6] = row(150, 0);
    e[7] = row(125, 3013);

    Bytes& g = f.game;
    g.push_back(0); put32(g, 0xA6C69C21); put32(g, 0xA8E36C34); put32(g, 8);
    put32(g, offs[0]); put32(g, 0xFFFFFFFE); put32(g, 0);   // the type row (c == 0)
    for (int i = 0; i < 7; ++i) { put32(g, offs[0]); put32(g, offs[i + 1]); put32(g, static_cast<uint32_t>(i + 1)); }
    // Two chunks: entries 0..3 and 4..7.
    Bytes chunks[2];
    for (int k = 0; k < 2; ++k) {
        Bytes u;
        for (int j = 0; j < 4; ++j) put16(u, 0);
        for (int j = 0; j < 4; ++j) {
            const uint16_t at = static_cast<uint16_t>(u.size());
            std::memcpy(&u[j * 2], &at, 2);
            u.insert(u.end(), e[k * 4 + j].begin(), e[k * 4 + j].end());
        }
        chunks[k] = fo::util::zlib_store(u.data(), u.size());
    }
    put32(g, 3);
    put32(g, 0); put32(g, 0);
    put32(g, 4); put32(g, static_cast<uint32_t>(chunks[0].size()));
    put32(g, 8); put32(g, static_cast<uint32_t>(chunks[0].size() + chunks[1].size()));
    put32(g, static_cast<uint32_t>(chunks[0].size() + chunks[1].size()));
    g.insert(g.end(), chunks[0].begin(), chunks[0].end());
    g.insert(g.end(), chunks[1].begin(), chunks[1].end());
    return f;
}

std::vector<float> ys(const Bytes& e) {
    std::vector<float> out;
    for (long at = fo::game::defs::find32(e.data(), e.size(), kPositionY); at >= 0;
         at = fo::game::defs::find32(e.data(), e.size(), kPositionY, at + 4)) {
        float f; std::memcpy(&f, &e[at + 4], 4); out.push_back(f);
    }
    return out;
}
}  // namespace

int main() {
    Fixture f = make();
    Bin bin;
    assert(bin.load(f.game, f.names));
    Bytes same;
    assert(bin.save(same) && same == f.game);  // untouched: byte for byte

    auto rep = fo::game::menu_row::patch(bin);
    if (!rep.ok) { std::printf("patch failed: %s\n", rep.error.c_str()); std::fflush(stdout); }
    assert(rep.ok);
    Bytes out;
    assert(bin.save(out));

    Bin back;
    assert(back.load(out, f.names));
    const Bytes& list = back.entry(back.find_def("PC_TOP_LEVEL_LIST"));
    const long ch = fo::game::defs::find32(list.data(), list.size(), kChildren);
    assert(fo::game::defs::rd32(&list[ch + 4]) == 4);
    const uint32_t want[] = {2, 4, 3, 6};  // Options, Mod Settings, Return To Game, ornament
    for (int i = 0; i < 4; ++i) assert(fo::game::defs::rd32(&list[ch + 8 + i * 4]) == want[i]);

    for (float y : ys(back.entry(4))) assert(y == 150 || y == 0);
    for (float y : ys(back.entry(3))) assert(y == 175 || y == 0);
    for (float y : ys(back.entry(6))) assert(y == 175 || y == 0);
    for (float y : ys(back.entry(2))) assert(y == 125 || y == 0);   // Options unchanged
    for (float y : ys(back.entry(7))) assert(y == 125 || y == 0);   // scoreboard row untouched

    const Bytes& r = back.entry(4);
    const long m = fo::game::defs::find32(r.data(), r.size(), kMessage);
    assert(fo::game::defs::rd32(&r[m + 4]) == fo::game::menu_row::kMessage);

    const Bytes& t = back.entry(5);
    const long tx = fo::game::defs::find32(t.data(), t.size(), kText);
    std::string s;
    for (size_t i = tx + 4; t[i] || t[i + 1]; i += 2) s.push_back(static_cast<char>(t[i]));
    assert(s == "Mod Settings");
    assert(fo::game::defs::rd32(&t[tx + 4 + 26]) == 0x51E278F0);  // the next field follows

    // A second run on the patched file refuses (the row is listed already).
    assert(!fo::game::menu_row::patch(back).ok);
    std::printf("menu_row tests passed\n");
    return 0;
}
