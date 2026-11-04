# Hero gear: the equipped weapons

Read statically from Steam Fable: The Lost Chapters (no ASLR, so the addresses are fixed).
The mod logs what it reads ("sync: Fable has the hero holding melee '...'"), to confirm in game.

## CTCInventoryWeapons

The hero's weapons inventory component, `CTCInventoryWeapons` (vtable `0x01253FF4`), is in the
hero's component list (`thing+0x44`/`+0x48`, `{type, component*}` pairs; see 12-renown.md).
Its save code (vtable slot 1, `0x005C3A95`) names the fields:

| Offset | What |
| --- | --- |
| +0x004 | owner thing (the hero) |
| +0x134 | handle to the active melee weapon thing |
| +0x13C | ActiveMeleeWeaponDefIndex2: the save copy of that thing's def index (written at save) |
| +0x140 | MeleeWeaponCarried (bool, read on load) |
| +0x141 | handle to the active ranged weapon thing (packed, unaligned) |
| +0x14C | ActiveRangedWeaponDefIndex2 |
| +0x150 | RangedWeaponCarried |
| +0x160 / +0x164 | confiscated melee / ranged def index |
| +0x168 / +0x16C | previously wielded melee / ranged def index |

At save time the code does `movzx eax, word [thing+0x8C]` on the weapon thing to get its def
index, so **thing+0x8C (uint16) is a thing's def index**.

A handle is 8 bytes; `[handle+4]` points at a ref block whose first dword is the thing
(`0x00A01B50`). A thing with `+0x91` bit 0 set is being removed (`0x004CC340`).

## Def index to name

The save code turns an index into a name with `0x0044C6B0` (returns `[0x013B879C]`, the def
manager), `0x009ACCC0` and `0x00415DD0`:

- entry = `[[defs + 0xA4] + index * 4]` (`+0xA4`/`+0xA8` are a vector's begin/end)
- name offset = `[entry + 4]` (-1: no name)
- text = `[0x013CA83C] + offset + 4`, NUL-terminated (`0x009D49B0`, the symbol table at
  `0x013CA828`, buffer begin/end at `+0x14`/`+0x18`)

## Not mapped yet

Worn clothing: `CTCInventoryClothing` (vtable `0x0124356C`) builds its list through a virtual
(`+0x158`) rather than fixed fields, so armour is not read yet. The server still uses its own
ledger for armour.

## Online

`client-mod/src/game/gear.cpp` reads the two weapon slots twice a second and sends
`{t:'gear', melee, ranged}` when they change. The server (`server/src/combat/client-gear.ts`)
fights with a reported weapon only if it is a known weapon, not staff-only, and owned in the
hero's ledger, or on a test realm with `FO_TRUST_CLIENT_GEAR=1`.
