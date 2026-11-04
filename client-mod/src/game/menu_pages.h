#pragma once
// Keeps the in-game menu's pages (Inventory, Quests, ..., Map, Stats, Magic) on the local hero.
//
// The player object ([0x013B8790]) holds the menu's pages in a manager at +0x32C: a vector of
// 11 page pointers at +4, the open page's index at +0x28. Each of the hero's CTCInventory*
// components puts its page there when it is set up (0x0066EB11, slot = the page's vfunc 2:
// Inventory 0, Quests 1, Experience 2, Abilities 3, Weapons 4, Clothing 5, Trade 6, Map 8,
// Stats 9, Magic 10) and takes it out when it is destroyed (0x0066EB78, only if the slot
// still holds that page).
//
// The stand-ins for other players are CREATURE_HERO, which carries the same components. Each
// one spawned put its own pages over the hero's, and when the stand-ins went away with the
// level, the slots were emptied. The next time the menu opened on a page (the Guild Seal
// opens the Map for its teleport list), Fable read an empty slot and crashed
// (CTCInventoryMap 0x006116E0, reading 0 at 0x0061173A).
//
// The fix routes the nine calls to the register function: a page only goes into an empty
// slot, or replaces the page already there when that one belongs to the same thing (never to
// a stand-in). The first hero to fill the slots is the local hero, who exists before any
// stand-in; a new game or a load empties the slots first through Fable's own unregister.
namespace fo::game::menu_pages {
/** Routes the calls; logs how many were changed. Safe to call more than once. */
bool install();
void remove();
/** Pages a stand-in tried to put over the hero's, for the status line. */
unsigned refused();
}  // namespace fo::game::menu_pages
