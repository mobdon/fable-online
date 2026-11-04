#pragma once
// The in-game chat box and name tags. Hooks Direct3D 9 (draw), the game window (typing)
// and DirectInput (so typing doesn't also move the hero).
#include "../game/hero.h"

namespace fo::overlay {
// From DllMain while Fable.exe is still suspended: patch the imports before the game
// creates its Direct3D device. Only touches the import table, so it's safe there.
void install_early();
// From the mod thread: logs what got hooked and falls back if the import patch missed.
void install_late();
// From the mod thread, a few times a second.
void set_hero(const game::HeroPos& pos);
void log_stats();
/** True while the chat box or the start-up menu has the keyboard and mouse. */
bool input_blocked_now();
}  // namespace fo::overlay
