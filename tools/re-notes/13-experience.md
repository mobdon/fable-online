# Hero experience and levels

Read statically from Steam Fable: The Lost Chapters (no ASLR, so the addresses are fixed).
Not yet checked in a running game: the mod logs what it reads (`sync: xp: ...`).

Fable has no character level. The hero earns general experience and Strength, Skill and
Will experience, and spends it on trainable stats and on abilities.

## CTCHeroExperience (vtable `0x0125F6C4`)

Its save code (vtable slot 1, `0x006D8E30`) names the fields:

| Offset | Save name | Type |
| --- | --- | --- |
| +0x0C | FishingLevel | int |
| +0x10 | TotalExperiencePoints | int |
| +0x14 | ExperiencePointsAvailableToSpend | int (general experience) |
| +0x18 | StatExperiencePoints | `vector<int>`, 3: Strength, Skill, Will |
| +0x24 | TrainableStatLevels | `vector<int>`, 10 |

- `0x006D6470` sets general experience (clamped at 0) and raises the total to match.
- `0x006D6590 (type, amount)` pays for a purchase: it takes `amount` from
  `StatExperiencePoints[type]`, and if that goes negative the rest comes out of general
  experience. So a purchase is paid from its own pool first, then from general.
- `0x006D6510 (i)` / `0x006D6520 (i)` read a stat pool / a stat level.

## Finding the components

Both are in the hero's sorted `{type, component*}` list at thing+0x44. Fable's own getters
look them up by type: `0x56FF3D` asks for type `0x68` (CTCHeroExperience), `0x56FFC5` for
type `0x6F` (CTCInventoryAbilities). A hero has more than 64 components, so a scan that
stops early misses them (online-17 logged "the experience component not found").

## Ability levels: CTCInventoryAbilities (vtable `0x01256AD4`)

`+0x160` points at 20 slots of `{int level, CHeroAbilityDef*}`. Level -1 means the hero
doesn't have it, 0 is level 1, up to 3 (`0x005EDF08` is "has ability": level >= 0;
`0x005EE01A` sets a level). The slots are filled at start-up (`0x005EEDE2`) by walking the
HERO_ABILITY defs: each def goes to the slot in its own `+0x3C` field. Several defs share
slot 18 (Summon, Divine Wrath, Unholy Power); the last one read wins.

A purchase (`0x0057862C` onwards) sets the level, then calls the pay function with the
def's `+0x40` as the pool (2 = Will when there is no def). That `+0x40` field hasn't been
matched to a game.bin field yet, so the server takes the pool from the def's name.

The save stores the levels as `AbilityLevels` (the slot levels in order).

## Price tables in game.bin

**MAIN_HERO_STATS** (group HERO_STATS) holds the ten trainable stat tracks in slot order.
Each track is `{0x1FA67C0C category (0 Strength, 1 Skill, 2 Will), 0xDCC01BA9 level count}`
followed by level rows of 24 bytes, `{0x273817D1 cost, 0xAFE941D7 ?, 0x80FF7E1D text id}`.
Level 0 costs 0 (the starting level), so there are 7 levels to buy. The text ids name them
(TEXT_GUI_EXPSPEND_PHYSICAL_STRENGTH_LEVEL1 and so on):

| Slot | Stat | Pool |
| --- | --- | --- |
| 0 | Physique | Strength |
| 1 | Health | Strength |
| 2 | Toughness | Strength |
| 3 | Speed | Skill |
| 4 | Accuracy | Skill |
| 5 | Guile | Skill |
| 6-8 | unused (no screen text) | Will |
| 9 | Will (magic power) | Will |

**HERO_ABILITY_*** defs (19): `0xAF220B38` is the slot, `0xFAFC8A1B` is `u32 4` + the
experience cost of each of the 4 levels, `0x0FDA6375` is `u32 4` + a per-level Will cost
(mana), `0xDF550E24` is the name text id.

## Experience in quest scripts

script.bin (CScriptDef SCRIPT_DEF) holds the experience quest scripts give, as named
values (`ArcheryExperience`, `PrisonEscapeExperienceFromMother`, ...). They are paid by the
local game, so online the server takes them as local gains, up to a daily cap.

## Online

`server/src/heroes/experience.ts` keeps the pools and levels; `npm run extract` reads the
price tables. The mod (`client-mod/src/game/hero_xp.cpp`, `world/xp_sync.cpp`) reports what
Fable changed by itself and writes the server's numbers back in.
