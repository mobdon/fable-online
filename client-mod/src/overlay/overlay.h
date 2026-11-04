#pragma once
// The in-game chat box and name tags. Hooks Direct3D 9 (draw), the game window (typing)
// and DirectInput (so typing doesn't also move the hero).
#include <array>
#include <map>
#include <string>
#include <vector>
#include "../game/hero.h"
#include "../ui/party_model.h"
#include "../ui/trade_model.h"

namespace fo::overlay {
// From DllMain while Fable.exe is still suspended: patch the imports before the game
// creates its Direct3D device. Only touches the import table, so it's safe there.
void install_early();
// From the mod thread: logs what got hooked and falls back if the import patch missed.
void install_late();
// From the mod thread, a few times a second.
void set_hero(const game::HeroPos& pos);
void log_stats();
/** True while the chat box, the trade window or the start-up menu has the keyboard and mouse. */
bool input_blocked_now();

// ---- player trading (ui/trade_model.h) ------------------------------------------------
// From the sync thread: a trade_* frame from the game connection.
void on_trade_frame(const std::string& json);
// From the sync thread when the game connection drops.
void on_world_disconnected();
// From the sync thread: the other heroes around, for "/barter" with no name.
void set_trade_targets(std::vector<ui::TradeTarget> targets);
// From the sync thread: trade requests to send on the game connection.
std::vector<std::string> take_world_outbox();
// Parties live on the world server, whose connection the game thread owns (sync.cpp).
/** A party frame from the world server, for the chat and the party list. Any thread. */
void post_world_frame(const std::string& json);
/** Party requests typed in the chat, to send over the world connection. Any thread. */
std::vector<std::string> take_world_requests();
/** Where party members' stand-ins are now, for the orbs over their heads. Any thread. */
void set_party_marks(std::vector<ui::PartyMark> marks);
/** Where other heroes' stand-ins are drawn now, so their name tags sit on them. Any thread. */
void set_tag_positions(std::map<uint32_t, std::array<float, 3>> live);
/** The world connection went down: the party list is no longer current. */
void clear_party();
/**
 * Darkens the whole screen (0 clear .. 1 black) with an optional line in the middle, over
 * the world and name tags but under the chat. Used while the hero is down. Any thread.
 */
void set_fade(float alpha, const std::string& caption);
/** Mouse buttons as Fable last read them through DirectInput (bit 0 = left). */
unsigned mouse_buttons();
}  // namespace fo::overlay
