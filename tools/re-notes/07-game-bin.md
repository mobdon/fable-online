# game.bin and names.bin (compiled defs)

Checked 2026-11-03 against the Steam install. The reader is `server/src/data/fable-bin.ts`;
the extractor that uses it is `server/src/data/extract.ts`.

## Layout

names.bin: `u32 size, u32 crc, u32 count, u32 ?, u32 0`, then `count` x `{u32 hash, char name[]}`.
Other files point at a name by its byte offset counted from the end of the 20-byte header.

game.bin:

| Part | Layout |
| --- | --- |
| Header | `u8 0` (uses names.bin), `u32` file id, `u32` names.bin crc (must match), `u32` entry count |
| Entry table | entry count x 12 bytes, in three runs: def types `{type, NULLDEF_type, 0}`, defs `{group, name, index in group}`, components `{type, 0xFFFFFFFF, index in type}` |
| Chunk table | `u32 n`, n x `{u32 first entry, u32 offset}` (last row is a terminator), `u32` compressed size |
| Data | zlib chunks. Each starts with one `u16` offset per entry it holds |

Entry index space = types, then defs, then components. A def reference stored in a value
is an entry index.

Every entry has 3 header bytes, then `{u32 field hash, value}` pairs. A def entry has a
`u16` component count and 12-byte rows `{u32 component type hash, u32 entry index, u32 owner def}`
before its own fields. The owner shows inheritance: a component owned by a `_TEMPLATE` def is shared.

Field hash: CRC-32, reflected, polynomial 0xEDB88320, init 0, **no final xor**
(`FRONT_END` -> `0x0A7D3E6C`). Some field names are still in Fable.exe and can be hashed;
most aren't, so unnamed fields are referred to by hash.

## Fields the server reads

| Where | Field | Type | How it was identified |
| --- | --- | --- | --- |
| CWeaponDef | `Type` | i32 | 0 sword, 1 axe, 2 hammer, 3 longbow, 4 crossbow |
| CWeaponDef | `Class` | i32 | 1 light, 2 heavy, 3 ranged |
| CWeaponDef | `Damage` | f32 | rises iron < steel < ebony < crystal < legendary; shown x10 in game |
| CStockItemDef | `0x70395B57` | i32 | base value in gold, rises with weapon tier |
| CAppearanceModifierDef | `0xDEBC9E21` | f32 | body coverage share (helmet, shirt, boots ... sum to about 1) |
| CAppearanceModifierDef | `0x909E89D5` | ref | the ARMOUR_HERO_* class of a clothing piece |
| CREATURE def | `Health` | f32 | |
| CREATURE def | `0xA4159D90` | f32 | grows with creature strength; used as melee power (unverified name) |
| CREATURE def | `0xBA0F59E8` | ref | the creature's weapon object |
| CREATURE def | `CombatTypeDef`, `GroupDef` | ref | |
| CCreatureDef | `0x2638684A` | i32 | rises grunt level 1 < 2 < 3 < leader < boss: experience |
| CCreatureDef | `0x277EDE57` | i32 | second reward value, used for loot gold (unverified meaning) |
| CEnemyDef | `Faction` | ref | FACTION_* |
| CREATURE_GENERATION_FAMILY | `0xC50C0369` | `u32 n` + n refs | the creatures a generator may pick |
| CQuestCardDef | `GoldReward`, `RenownReward` | i32 | |
| CQuestCardDef | `RewardObjects` | `u32 n` + n refs | |
| SPECIAL_ABILITIES_* | many | `u32 4` + 4 x f32 | per-level spell tables (heal amounts are `0x2DDA34F8` in HEAL_LIFE) |

Not found: a numeric armour or defence rating on hero clothing or on the ARMOUR_* defs
(those hold hit effects and sounds only).

## Level files

`data/Levels/FinalAlbion.wad` is a `BBBB` archive of `.lev` and `.tng` files (index at the
offset in the header). `.tng` files are plain text `NewThing ... EndThing` blocks.
`tools/extractor/spawns.mjs` keeps `AICreature` things and `MARKER_CREATURE_GENERATOR`
markers (families, radius, limits). Positions are level-local metres, z up.
