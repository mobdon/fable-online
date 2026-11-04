# Hero health

Read statically from Steam Fable: The Lost Chapters (no ASLR, so the addresses are fixed).
Not yet seen live in a debugger; the mod logs both numbers on F7 (dev builds) to confirm.

## Where it lives

Health is on the thing itself (`CThing`, so the hero's `CThingPlayerCreature` and every
other creature), not on `CTCHeroStats`.

| Offset | What |
| --- | --- |
| +0x091 | flags byte; bit 0 set means the thing is being removed (the getters return nothing) |
| +0x0B0 | max health (float) |
| +0x0B4 | current health (float) |
| +0x0BC | flags; bit 0 lets damage through, bit 1 (with ModifyHealth's bool) floors health at 1 |

`CTCHeroStats` +0x058/+0x05C (1000/1000) are something else.

## How it was found

`CGameScriptInterface` (vtable 0x01260F0C, see 04-script-interface.md):

- slot +0x404, `0x00890370`, hero health: main player `0x00449970`, its thing
  `0x00487DC0` (`CPlayer`+0x2C), skip if `[thing+0x91] & 1`, then `fld [thing+0xB4]`.
- slot +0x408, `0x008903A0`, hero max health: the same, `fld [thing+0xB0]`.
- slot +0x41C, `0x00891720`, GiveHeroHealth: calls the thing's vtable slot +0xEC.

## Changing it the engine's way

Thing vtable slot +0xEC (`0x006A68F0` on the hero's vtable 0x012457FC) is ModifyHealth
`(float delta, bool)`, `__thiscall`, `ret 8`. It rounds the delta to a whole number and calls
`0x006A62E0`, which:

1. Ignores damage unless `+0xBC` bit 0 is set (and two global switches for heroes/creatures).
2. Adds the delta to +0xB4, applies armour-style reductions for some things.
3. Floors at the def's minimum (`[[thing+0x70]+0x54]`), at 0 (or 1 when `+0xBC` bit 1 is
   set), and caps at +0xB0.
4. At 0 runs the death path (`0x004C7C40` for creatures).

## What the mod does

`client-mod/src/game/thing.cpp` reads +0xB0/+0xB4 and writes +0xB4 directly, which skips
step 4 on purpose: death belongs to the server (a downed state with its own revive). Each
tick, `sync.cpp` sets the local hero to the server's share of max health
(`world/health.cpp`): server 40 of 160 is 25% of Fable's own max. Fable's max is not touched,
because the hero's stats code owns it. A hero the server has down is held at 1.

Only tick frames whose own-hero entry carries `max_health` are applied, which today means
regions running the combat sim.

## Local heals (potions, food, Heal Life)

Fable's item use and spell casting are not mapped yet, so the mod watches for the hero's
health going up without its own write (`world::HealWatch`). Rises within 300 ms are gathered
into one heal and sent as `{"t":"heal","share":<of Fable's max>,"key":...}`. While the server
answers (1.5 s) the mod does not lower the local health, so there is no flicker.

The server (`server/src/combat/healing.ts`) never takes the number as health. It lists what
the hero could have used: Heal Life if castable now, and every healing item carried in the
inventory (not the bank). It picks the one whose heal is nearest the reported share (within
a factor of 2), spends it (Will and cooldown, or one item from the ledger, once per key) and
heals by the server's amount. Otherwise it refuses, and the mod puts the health back.

The item heal shares in `HEAL_ITEMS` are first guesses. The analytics `heal` event logs the
share each local heal reported, so they can be tuned to Fable's real amounts.

## Still open

- Read Fable's real potion and food heal amounts from game.bin instead of guessing.
- Mapping item use and casting directly would remove the guess (and fix the local
  inventory count, which Fable changes on its own today).
- Will: probably next to health on the thing or on CTCHeroStats; not looked at yet.
- Showing the server's downed state (kneel animation) instead of holding at 1.
