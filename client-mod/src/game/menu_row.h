#pragma once
// Adds a "Mod Settings" row to Fable's in-game menu (Inventory .. Return To Game) by reusing
// the menu row Lionhead left in game.bin but never listed: PC_MENU_ENTRY_MSNCHAT, with its
// button PC_MSNCHAT_BUTTON and label UI_MSNCHAT_TEXT (a literal string, "Msn Chat").
//
// The menu is PC_TOP_LEVEL_LIST (UI def, Type 8): its Children field (0x3DC30C85, a count
// and entry indices) lists the rows in screen order, then the two ornaments. Each row places
// itself with PositionY (0x69DCBAA7, once per state block) on a pitch of 25: Inventory 0 ..
// Options 125, Return To Game 150, the bottom ornament 150. The patch:
//  - lists MSNCHAT before Return To Game, at 150, and moves Return To Game and the bottom
//    ornament to 175;
//  - sets the row's message (field 0xB40B9CDE, the value a row sends when picked; Inventory
//    238, Map 254 ...) to kMessage, a number Fable's message switch leaves unhandled (0xBBD,
//    between 0xBBC and 0xBC3, goes to the switch's default), which the DLL catches;
//  - writes the label "Mod Settings" in place of "Msn Chat" (field 0xE215EF13, UTF-16).
//
// No Windows or game dependencies: tested on its own (tests/menu_row_test.cpp).
#include <cstdint>
#include <string>

namespace fo::game::defs { class Bin; }

namespace fo::game::menu_row {

constexpr uint32_t kMessage = 0xBBD;  // 3005
constexpr const char* kAlreadyListed = "the menu already lists the row";

struct Report {
    bool ok = false;
    std::string error;
};

Report patch(defs::Bin& bin, const char* label = "Mod Settings");

}  // namespace fo::game::menu_row
