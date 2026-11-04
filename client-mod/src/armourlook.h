#pragma once
// Test-only armour recolour. With FABLE_ONLINE_ARMOUR_LOOK=1 set, the hero's plate armour
// textures are recoloured from the player's own textures.big as the game loads them.
// See armourlook.cpp.
namespace fo::armourlook {
// Call from DllMain after fo::herolook::install(), before the game is resumed.
void install();
}
