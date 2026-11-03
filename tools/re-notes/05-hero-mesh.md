# Hero mesh

The hero is drawn with one animated mesh, `MESH_HERO` (id 4299, type 5), in bank `MBANK_ALLMESHES` of
`data/graphics/graphics.big`. Its source path is `Art3\Hero\UnclothedHero`; clothing and armour are separate
meshes (`MESH_HERO_<SET>_<PART>_01`) attached on top.

All of these use a 3ds Max `Bip01` biped rig with the same helper points (Scene Root, Movement_dummy, WEAPON,
SCABBARD, EYE_SET_L/R):

| Mesh | Id | Data size | Sub-header |
| --- | --- | --- | --- |
| `MESH_HERO` | 4299 | 129,050 | 108 |
| `MESH_RIVAL_HERO_WHISPER` | 7659 | 216,555 | 104 |
| `MESH_RIVAL_HERO_APPRENTICE_FEMALE` | 7651 | 128,801 | 100 |
| `MESH_RIVAL_HERO_BRIAR_ROSE` | 7655 | 266,908 | 124 |
| `MESH_RIVAL_HERO_MAZE` | 7654 | 222,465 | 108 |

## Bank entry table

`BIGB` header: magic, u32 version, u32 bank index offset. Bank index: u32 count, then per bank `name\0`, u32 id,
u32 entry count, u32 table offset, u32 table size, u32. Entry table: u32 n, n x 8 bytes, then per entry:
u32 magic (0x2A), id, type, data size, data offset, crc, name length, name, u32 crc, u32 source count,
(u32 length, path) per source, u32 sub-header length, sub-header.

## Model swap test

`client-mod/src/herolook.cpp` (on with `FABLE_ONLINE_HERO_MESH=<mesh name>`) rewrites `MESH_HERO`'s entry as the
game reads the table, keeping its id and name and taking type, size, offset, crc and sub-header from the named
mesh. The new entry must not be longer than the old one, so Briar Rose (124-byte sub-header) is refused.

Still open: whether hero animations and `CTCHeroMorph` (age, weight, strength, alignment) behave on the swapped
mesh, and how the male armour meshes sit on it.
