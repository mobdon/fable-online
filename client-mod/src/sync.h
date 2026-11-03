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

/** F6 and friends while FABLE_ONLINE_DEV=1; does nothing otherwise. */
void dev_key(int vk);

}  // namespace fo::sync
