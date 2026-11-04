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
/** Asks the server to remove the nearest monster (within 50 m) or every monster within 30 m.
 *  Staff accounts only; removed for every player. */
void dev_despawn(bool all_near);

/**
 * The hero's renown for the own name tag: what the server keeps when it has answered a
 * report, otherwise the number read from Fable itself; -1 before either is known.
 */
int own_renown();
/**
 * True while the world is on screen and being played: the game thread ticked in the last
 * 250 ms, there is a hero, and no region or area load (or its fade) is under way. Name tags
 * are drawn only then. Any thread.
 */
bool world_shown();

}  // namespace fo::sync
