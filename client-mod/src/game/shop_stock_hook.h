#pragma once
// Serves Fable a game.bin with the online realm's shop rules applied (see shop_stock.h).
// The filtered copy is built from the player's own file the first time the game opens it,
// written next to fable_online.dll in cache\, and the game's open of
// data\CompiledDefs\game.bin is pointed at it. Nothing from the game is shipped.
#include <windows.h>

namespace fo::game::shop_stock {

/** Hooks CreateFileA/W in Fable.exe's import table. Call from DllMain, before the game runs. */
void install(HMODULE self);
/** One log line: whether the filtered game.bin was served and the save-load check patched. */
void log_status();

}  // namespace fo::game::shop_stock
