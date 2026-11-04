#include "menu_row.h"

#include <cstring>
#include <vector>

#include "defs_edit.h"

namespace fo::game::menu_row {
namespace {
constexpr uint32_t kChildren = 0x3DC30C85, kPositionY = 0x69DCBAA7, kMessageField = 0xB40B9CDE, kText = 0xE215EF13;

uint32_t f32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; }

// Sets every PositionY equal to `from` to `to`. Returns how many changed.
int move_y(std::vector<uint8_t>& e, float from, float to) {
    int n = 0;
    for (long at = defs::find32(e.data(), e.size(), kPositionY); at >= 0 && size_t(at) + 8 <= e.size();
         at = defs::find32(e.data(), e.size(), kPositionY, at + 4)) {
        if (defs::rd32(&e[at + 4]) == f32(from)) { defs::wr32(&e[at + 4], f32(to)); ++n; }
    }
    return n;
}
}  // namespace

Report patch(defs::Bin& bin, const char* label) {
    Report r;
    auto fail = [&](std::string why) { r.error = std::move(why); return r; };
    const int list = bin.find_def("PC_TOP_LEVEL_LIST"), row = bin.find_def("PC_MENU_ENTRY_MSNCHAT"),
              resume = bin.find_def("PC_MENU_ENTRY_RESUME"), ornament = bin.find_def("PC_TOP_LEVEL_ORNAMENT_BOTTOM"),
              text = bin.find_def("UI_MSNCHAT_TEXT");
    if (list < 0 || row < 0 || resume < 0 || ornament < 0 || text < 0) return fail("a menu def is missing");

    // Check everything before changing anything.
    const auto& le = bin.entry(list);
    const long ch = defs::find32(le.data(), le.size(), kChildren);
    if (ch < 0 || size_t(ch) + 8 > le.size()) return fail("PC_TOP_LEVEL_LIST has no Children");
    const uint32_t n = defs::rd32(&le[ch + 4]);
    if (n == 0 || n > 64 || size_t(ch) + 8 + n * 4 > le.size()) return fail("PC_TOP_LEVEL_LIST Children look wrong");
    int resume_at = -1;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t c = defs::rd32(&le[ch + 8 + i * 4]);
        if (c == uint32_t(row)) return fail(kAlreadyListed);
        if (c == uint32_t(resume)) resume_at = static_cast<int>(i);
    }
    if (resume_at < 0) return fail("Return To Game isn't in the menu");
    const auto& re = bin.entry(row);
    const long msg = defs::find32(re.data(), re.size(), kMessageField);
    if (msg < 0 || size_t(msg) + 8 > re.size()) return fail("the row has no message field");
    const auto& te = bin.entry(text);
    const long tx = defs::find32(te.data(), te.size(), kText);
    if (tx < 0) return fail("the label has no text field");
    size_t tend = size_t(tx) + 4;
    while (tend + 2 <= te.size() && (te[tend] || te[tend + 1])) tend += 2;
    if (tend + 2 > te.size()) return fail("the label's text isn't terminated");
    {
        std::vector<uint8_t> probe = bin.entry(row), probe2 = bin.entry(resume), probe3 = bin.entry(ornament);
        if (!move_y(probe, 125, 150) || !move_y(probe2, 150, 175) || !move_y(probe3, 150, 175))
            return fail("row positions aren't where expected");
    }

    // The list: one more child, before Return To Game.
    {
        auto& e = bin.edit(list);
        defs::wr32(&e[ch + 4], n + 1);
        uint8_t v[4];
        defs::wr32(v, uint32_t(row));
        e.insert(e.begin() + ch + 8 + resume_at * 4, v, v + 4);
    }
    // Positions.
    move_y(bin.edit(row), 125, 150);
    move_y(bin.edit(resume), 150, 175);
    move_y(bin.edit(ornament), 150, 175);
    // Message.
    defs::wr32(&bin.edit(row)[msg + 4], kMessage);
    // Label.
    {
        auto& e = bin.edit(text);
        std::vector<uint8_t> s;
        for (const char* p = label; *p; ++p) { s.push_back(static_cast<uint8_t>(*p)); s.push_back(0); }
        e.erase(e.begin() + tx + 4, e.begin() + tend);
        e.insert(e.begin() + tx + 4, s.begin(), s.end());
    }
    r.ok = true;
    return r;
}

}  // namespace fo::game::menu_row
