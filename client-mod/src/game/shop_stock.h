#pragma once
// Takes items the online realm doesn't sell out of Fable's own shops.
//
// Each shop's stock comes from its CShopDef in data/CompiledDefs/game.bin: rows of `Type`
// (the item def), Quantity, PriceMultiplier, MaxStock and RestockPeriod. A row with Quantity
// and MaxStock both 0 is an empty slot, which the retail file already uses. This rewrites the
// rows for banned items that way and writes a new game.bin, so the shop never holds them.
//
// Banned: OBJECT_RESURRECTION_PHIAL, and hero clothing whose armour class
// (CAppearanceModifierDef field 0x909E89D5) is ARMOUR_HERO_PLATE_ARMOUR or
// ARMOUR_HERO_PLATINUM_ARMOUR. The server's catalog has the same rule (NOT_SOLD_ARMOUR,
// UNSHOPPABLE, staff armour); this keeps the game's shop screen in line with it.
//
// Layout notes: tools/re-notes/07-game-bin.md. Only changed chunks are re-encoded (as stored
// zlib blocks); every other byte of the file is kept.
//
// No Windows or game dependencies: tested on its own (tests/shop_stock_test.cpp).
#include <cstdint>
#include <string>
#include <vector>

namespace fo::game::shop_stock {

struct Report {
    bool ok = false;
    std::string error;              // why the file was left alone
    unsigned shops = 0;             // CShopDef components seen
    unsigned rows_cleared = 0;      // stock rows emptied
    unsigned banned_defs = 0;       // item defs matched by the rule
    unsigned chunks_rewritten = 0;
    std::vector<std::string> cleared;  // names of the items whose rows were emptied (unique)
};

/** Fable's field-name hash: reflected CRC-32, polynomial 0xEDB88320, init 0, no final xor. */
uint32_t fable_hash(const char* s);

/**
 * Returns the filtered game.bin in `out`. When nothing needs changing, `out` is a copy of the
 * input and report.rows_cleared is 0. On any parse error the report says why and `out` is empty.
 */
Report filter(const std::vector<uint8_t>& game_bin, const std::vector<uint8_t>& names_bin, std::vector<uint8_t>& out);

}  // namespace fo::game::shop_stock
