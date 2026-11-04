#pragma once
// Target highlight colour. Fable's targeting highlight (NPlayerGui::CDrawHighlightingTargetingCursor,
// update at 0x0064BFBA) sorts the targeted thing into one of five kinds and takes the glow
// colour for that kind from a table in the player GUI definition (+0x86C start colours,
// +0x878 end colours). Kind 2 is an enemy; villagers and other creatures get other kinds.
// With [display] red_target on, a creature the hero targets while in combat mode (weapon or
// spell out) is shown as kind 2. Sheathing the weapon brings the game's own colour back.
namespace fo::display::target_colour {
void install();
const char* status();
}  // namespace fo::display::target_colour
