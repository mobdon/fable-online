#pragma once
// Entity sync: the mod's side of "show me what the server sees".
//
// It owns the game connection, the entity table and the puppets, and runs the loop that
// turns the server's frames into creatures standing in the player's own Albion. Other
// players and the server's monsters both come through here.
namespace fo::sync {

/** Reads its settings from the launcher's environment variables and starts the loop. */
void start();
void stop();

/**
 * Dev tools, from the Developer page of Mod settings (and F6-F8 when [dev] hotkeys=1):
 * VK_F6 toggles the local game's own spawning, VK_F7 writes the online status to the log,
 * VK_F8 spawns or removes a test stand-in. Game thread only; does nothing before start().
 */
void dev_key(int vk);
/** Asks the server for `count` monsters ahead of the hero (`def` empty = the server picks).
 *  Staff accounts only; the answer shows on the Mod settings screen and in the log. */
void dev_spawn(const char* def, int count);
/** Staff accounts only: moves the realm's clock forward to `hour` (0..24) for every player. */
void dev_time(float hour);
/** Staff accounts only: `weather` ("rain", "storm", ...) on every outdoor region for 30 minutes; "auto" ends it. */
void dev_weather(const char* weather);

}  // namespace fo::sync
