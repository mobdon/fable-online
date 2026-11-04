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
// The fix routes the nine calls to the register function. A page is refused when its owner
// (the component at page - 0x130, its thing at +4) is not a CThingPlayerCreature, which keeps
// every stand-in out. A second player creature also can't replace a page still held by the
// live local hero; after a load or region change the old hero is gone and the new hero's
// pages register as normal. Components are set up in the thing's init (0x006A9DD0), after
// the constructor has set the final vtable, so the owner's class is known at that point.
namespace fo::game::menu_pages {
/** Routes the calls; logs how many were changed. Safe to call more than once. */
bool install();
void remove();
/** Pages a stand-in tried to put over the hero's, for the status line. */
unsigned refused();
}  // namespace fo::game::menu_pages
