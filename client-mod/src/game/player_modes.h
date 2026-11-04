#pragma once
// Which of the player's modes are running: Fable keeps a stack of player modes (normal play,
// combat, the in-game menu, conversations...). The mode handler table at 0x013B8C88 holds,
// per mode id (0x20 bytes each), a member function run when the mode starts (+0x00) and one
// run when it ends (+0x10). Every entry that points into Fable's code gets a thunk that notes
// the change and jumps on to Fable's own handler (the same scheme as pause_menu, which keeps
// mode 23). Used to find Fable's menus, and logs each change so the menu's mode id can be read
// from fable_online.log.
#include <cstdint>
namespace fo::game::player_modes {
/** Patches the table once it is filled. Cheap after that: call it every frame. */
void poll();
/** True while mode `id` (0..63) has started and not ended. */
bool running(int id);
/** Bit i set while mode i runs. */
uint64_t running_mask();
}  // namespace fo::game::player_modes
