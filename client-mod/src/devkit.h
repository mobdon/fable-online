#pragma once

namespace fo::devkit {
// Test-only tools, on when FABLE_ONLINE_DEV=1. Call from the mod thread; it waits for the game
// window and then handles hotkeys on the game's own thread.
void install();
}  // namespace fo::devkit
