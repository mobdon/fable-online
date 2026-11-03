# Test hero

The test hero starts from a save made just after Guild graduation. The mod then adds gear and stats on top.

## Gear (def names from data/CompiledDefs/names.bin)

- Sword of Aeons: `OBJECT_SWORD_OF_AEONS` (also `_GOOD_01`; `_FOR_JACK` is Jack's copy).
- Platinum armour set (Lost Chapters): `OBJECT_HERO_HELMET_PLATINUM`, `OBJECT_HERO_SHIRT_PLATINUM`,
  `OBJECT_HERO_GLOVES_PLATINUM`, `OBJECT_HERO_TROUSERS_PLATINUM`, `OBJECT_HERO_BOOTS_PLATINUM`.

## Fable's console

The console singleton is at `[0x013CAA40]` (vtable 0x0129C600). It has 43 commands, registered through
0x009EC5E0. They include ActivateQuest, SetLevel, SetStartingHolySite, SetTimeOfDay, RunScript and CommandList.
None of them gives items, sets stats or skips quests, so gear needs Fable's script interface instead.
