# Hero live state

Found with the probe (build 4) on Steam Fable: The Lost Chapters. Fable.exe has no ASLR, so vtables are fixed.

## Hero (CThingPlayerCreature, vtable 0x012457FC)

| Offset | What |
| --- | --- |
| +0x060 | pointer to CTCPhysicsControlled (vtable 0x0126616C) |
| +0x064 | pointer to CTCGraphicAppearance |
| +0x0E8..+0x0F0 | region anchor position; changes only when the area changes |

## CTCPhysicsControlled (hero+0x60)

| Offset | What |
| --- | --- |
| +0x00C, +0x010, +0x014 | live position x, y, z (float) |
| +0x018, +0x01C, +0x020 | position from the last physics step |
| +0x024 | game frame of the last update (int) |
| +0x050..+0x060 | orientation: up vector, then forward x, y |
| +0x068..+0x078 | orientation from the last physics step |
| +0x080 | game frame counter (int, about 15 a second) |

The position moved smoothly while the hero walked, and jumped when the hero went through an area change.

## Still open

- Health: found, on the thing itself (+0xB4 current, +0xB0 max). See 11-hero-health.md.
