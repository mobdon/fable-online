#pragma once
// Test-only hero model swap. With FABLE_ONLINE_HERO_MESH=<mesh name> set, the hero is drawn with
// that mesh from the player's own graphics.big instead of MESH_HERO. See herolook.cpp.
namespace fo::herolook {
// Call from DllMain, before the game is resumed: it must be in place before graphics.big opens.
void install();
}
