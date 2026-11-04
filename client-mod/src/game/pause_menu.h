#pragma once
// Fable's pause screen (Start / Esc: Return to game, Save, Load, Options, Quit).
// The pause screen is player mode 23. The player's mode handlers sit in a table at 0x013B8C88,
// 0x20 bytes per mode id: +0x00 a member function run when the mode starts, +0x10 one run when
// it ends. Mode 23's are 0x00636270 (builds UI_TOP_LEVEL_PAUSE_SCREEN through 0x004830A1 and
// pushes CInputProcessInGameMenu) and 0x006362D0 (closes it through CTCInGameMenu 0x0047D78B).
// The mod swaps those two table entries for thunks that note the change and jump on to Fable's
// own handler, so nothing in Fable's code is changed. The table is filled by a static
// initialiser, so the swap is retried until the entries hold Fable's handlers.
namespace fo::game::pause_menu {
void install();
// True while Fable's pause screen (or a screen opened from it, such as Options) is up.
// Cheap: call it every frame.
bool open();
const char* status();
}  // namespace fo::game::pause_menu
