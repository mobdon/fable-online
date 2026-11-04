#pragma once
namespace fo::devkit {
// Test-only tools, on when FABLE_ONLINE_DEV=1. Call install() from the mod thread; it waits for
// the game window, then runs god mode (and the hotkeys when [dev] hotkeys=1) on the game thread.
void install();
// The Developer page's actions. Game thread only.
void test_kit();          // Sword of Aeons, the platinum set (worn), 100,000 gold and a full heal
void give_gold(int amount);
void heal_full();
void log_health();
}  // namespace fo::devkit
