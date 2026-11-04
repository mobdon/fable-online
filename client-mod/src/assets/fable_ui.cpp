#include "fable_ui.h"
#include "../../../shared/art/big.h"

namespace fo::assets {
namespace {
template <class Path>
UiArt load(const Path& fonts_path, const Path& frontend_path, const Path& textures_path) {
    UiArt ui;
    std::string err;
    art::Bank fonts;
    if (fonts.open(fonts_path)) {
        std::vector<uint8_t> raw;
        auto one = [&](const char* name, Font& f) {
            return fonts.load_raw(name, raw) && parse_font(raw, f, &err);
        };
        ui.fonts_ok = one(kFontChat, ui.chat) && one(kFontSmall, ui.small_font) && one(kFontTitle, ui.title);
        if (!ui.fonts_ok) ui.report += "fonts.big unreadable (" + (err.empty() ? fonts.error() : err) + "); ";
    } else {
        ui.report += "fonts.big not found; ";
    }
    art::Bank fe;
    if (fe.open(frontend_path)) {
        struct Piece { const char* name; art::Image* img; } pieces[] = {
            {"UI_TABLE_TEST_TL_FE", &ui.tl}, {"UI_TABLE_TEST_TR_FE", &ui.tr},
            {"UI_TABLE_TEST_BL_FE", &ui.bl}, {"UI_TABLE_TEST_BR_FE", &ui.br},
            {"UI_TABLE_TEST_H_T_FE", &ui.top}, {"UI_TABLE_TEST_H_B_FE", &ui.bottom},
            {"UI_TABLE_TEST_V_L_FE", &ui.left}, {"UI_TABLE_TEST_V_R_FE", &ui.right},
        };
        ui.frame_ok = true;
        for (auto& pc : pieces) {
            art::Texture t;
            if (!fe.load(pc.name, t) || !art::decode(t, *pc.img) || pc.img->empty()) {
                ui.frame_ok = false;
                ui.report += std::string(pc.name) + " missing; ";
                break;
            }
        }
        // Scroll bars: rows 16..49 of the 64px textures hold the bar and its ornate ends.
        const char* bars[2][3] = {{"FE_BUTTON_OFF_L", "FE_BUTTON_OFF_M", "FE_BUTTON_OFF_R"},
                                  {"FE_BUTTON_ON_L", "FE_BUTTON_ON_M", "FE_BUTTON_ON_R"}};
        ui.bar_ok = true;
        for (int on = 0; on < 2 && ui.bar_ok; ++on)
            for (int i = 0; i < 3; ++i) {
                art::Texture bt;
                art::Image full;
                art::Image& dst = on ? ui.bar_on[i] : ui.bar_off[i];
                if (!fe.load(bars[on][i], bt) || !art::decode(bt, full) || full.h < 50) { ui.bar_ok = false; break; }
                dst.w = full.w;
                dst.h = 33;
                dst.px.assign(full.px.begin() + 16 * full.w, full.px.begin() + 49 * full.w);
            }
        if (!ui.bar_ok) ui.report += "menu bar art missing; ";
        art::Texture t;
        art::Image back;
        if (fe.load("UI_TABLE_BACK_SPRITE_FE", t) && art::decode(t, back) && !back.empty())
            ui.fill = back.px[back.px.size() / 2] | 0xFF000000;
    } else {
        ui.report += "frontend.big not found; ";
    }
    // The chat window's frame. textures.big is large (about 500 MB) but only its entry table
    // and these nine small sprites are read.
    art::Bank tx;
    if (tx.open(textures_path)) {
        ui.msn_ok = true;
        std::string sizes;
        for (int i = 0; i < 9 && ui.msn_ok; ++i) {
            std::string name = "PC_UI_MSN_FRAME_" + std::to_string(i + 1) + "_SPRITE";
            art::Texture t;
            if (!tx.load(name, t) || !art::decode(t, ui.msn[i]) || ui.msn[i].empty() || ui.msn[i].w > 256 || ui.msn[i].h > 256) {
                ui.msn_ok = false;
                ui.report += name + " not readable (" + tx.error() + "); ";
                break;
            }
            sizes += (i ? " " : "") + std::to_string(ui.msn[i].w) + "x" + std::to_string(ui.msn[i].h);
        }
        if (ui.msn_ok) ui.report += "chat window frame " + sizes + "; ";
    } else {
        ui.report += "textures.big not found (" + tx.error() + "); ";
    }
    if (ui.report.empty()) ui.report = "Fable fonts and frame loaded";
    return ui;
}
}  // namespace

UiArt load_ui(const std::wstring& dir) {
    return load(dir + L"\\data\\lang\\English\\fonts.big", dir + L"\\data\\graphics\\pc\\frontend.big",
                dir + L"\\data\\graphics\\pc\\textures.big");
}

UiArt load_ui_utf8(const std::string& dir) {
    return load(dir + "/data/lang/English/fonts.big", dir + "/data/graphics/pc/frontend.big",
                dir + "/data/graphics/pc/textures.big");
}

}  // namespace fo::assets
