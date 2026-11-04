#pragma once
#include <string>
#include <vector>
#include "../ui/party_model.h"
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

// Parties live on the world server, whose connection the game thread owns (sync.cpp).
/** A party frame from the world server, for the chat and the party list. Any thread. */
void post_world_frame(const std::string& json);
/** Party requests typed in the chat, to send over the world connection. Any thread. */
std::vector<std::string> take_world_requests();
/** Where party members' stand-ins are now, for the orbs over their heads. Any thread. */
void set_party_marks(std::vector<ui::PartyMark> marks);
/** The world connection went down: the party list is no longer current. */
void clear_party();
}  // namespace fo::overlay
