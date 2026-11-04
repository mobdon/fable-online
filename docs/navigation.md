# Walkable ground (server navmesh)

The server runs creatures itself, so it needs to know where the ground is. Fable already
ships that data: every level file carries a heightmap and the navigation quadtree the
game's own AI paths on. `tools/extractor/navmesh.mjs` reads it from the player's install
and writes one file per region to `shared/defs/nav/<LevelScriptName>.json` (git-ignored:
derived from game files, never committed).

```
node tools/extractor/navmesh.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
# 130 regions, 252517 walkable cells -> shared/defs/nav (268 backdrop levels with no ground)
```

It also writes `shared/defs/nav-regions/<Region>.json`: every level of a server region
joined into the region's frame (the anchor level's, the frame spawns and things use), with
cells of neighbouring levels linked where they meet. A region file carries `originX/originY`,
the region-frame position of its heightmap corner. `ops/local-host` builds these on setup and
points the server at them (`FO_NAV_DIR=../shared/defs/nav-regions`), so server monsters
path round Fable's own walls and stand on its ground.

Start the server with `FO_NAV_DIR=../shared/defs/nav` to run those regions on
`NavSim` (heroes held to the ground, creatures run by the AI). Without it every region
stays on the kinematic `StubSim`. `npm run ai-bench -- HeroGuildComplex 300 64` is a load
test: 300 creatures and 64 heroes tick in about 1 ms (p50) and 7.5 ms (p99).

## Where it comes from

`data/Levels/FinalAlbion.wad` is a `BBBB` archive holding 398 `.lev` files (terrain and
navigation) and 398 `.tng` files (placed things, plain text). 130 levels have ground; the
rest are sea, filler and vista backdrops with an empty navigation section.

### FinalAlbion.wad

| Offset | Type | Field |
| --- | --- | --- |
| 0 | char[4] | `BBBB` |
| 4 | u32 x3 | version |
| 16 | u32 | block size (0x800) |
| 20 | u32 | file count |
| 24 | u32 | file count again |
| 28 | u32 | offset of the file table |

Table: u32 x3 header, then per file: u32 marker, u32 id, u32 0, u32 size, u32 offset,
u32 0, u32 name length, name (`Data\Levels\FinalAlbion\X.lev`), u32 0, u32 1, u32 0,
u32 n, n bytes (three date stamps and a checksum).

### .lev

| Offset | Type | Field |
| --- | --- | --- |
| 0 | u32 | header size (25) |
| 4 | u16 | version |
| 13 | u32 | obsolete-data offset |
| 21 | u32 | navigation offset (from the start of the file) |
| 38 | u32 | width in cells |
| 42 | u32 | height in cells |

After the ground-theme and sound-theme palettes (the last sound theme name ends just
before the first heightmap cell) come `(width+1) * (height+1)` heightmap cells of 21
bytes each: u32 size (21), u8 version, f32 height, u8, u8 x3 ground themes, u8 x2 theme
strengths, u8 path flag, u8 passover, u8 sound theme, u8, u8 shore, u8.

- Height in metres = stored value x 2048. Checked against `.tng` marker positions on
  Lookout Point: within 0.3 m.
- Cells are 1 m apart, so a 128 x 128 level is 128 m across, in the same local frame the
  `.tng` files use (x/y across, z up, origin at the corner).
- The "walkable" byte other tools name is really a road flag (it marks paths, not grass),
  so the navigation section is the source of truth.

### Navigation section

At the navigation offset: u32 sections start, u32 section count, then per section a
length-prefixed name (`NULL` for the main ground) and u32 start. Each section:

| Type | Field |
| --- | --- |
| u32 | size |
| u32 | version |
| f32 x2 | level width and height in metres |
| u32 | quadtree layers |
| u32 n, n x (f32 x, f32 y, u32 subset) | interactive points |
| u32 | subset count |
| u32 | node count, then the nodes |

Every node starts with three bytes `[exit, root, blank]`:

- `blank` set and `exit` clear: a 32 m root block with no ground at all. Nothing follows.
- otherwise three more bytes `[leaf, layer, subset]`, then f32 x, f32 y (centre), u32 id.
  A cell is `32 / 2^layer` metres across (layer 0 = 32 m ... layer 6 = 0.5 m).
  - `leaf` clear: an inner node, followed by four u32 child ids (0 = no ground there).
  - `leaf` set: walkable ground. u32 z-level, u8 flags, u32 n, n u32 ids of the leaves it
    touches. Exit leaves (`exit` set) then add u32 n and n u64 UIDs of the region-exit
    things beside them; the full UID is the stored value + 18446741874686296064, which
    matches the UIDs in the `.tng`.

Every one of the 130 levels with ground parses to the last byte (each section ends 4
bytes before the end of the file), and on Lookout Point every adjacency is two-way.

## Server side

`server/src/world/nav/navmesh.ts`:

- `walkable(x, y)`, `cellAt(x, y)`: through a 1 m bucket grid, O(1).
- `heightAt(x, y)`: bilinear on the heightmap.
- `findPath(a, b)`: A* over the leaves, through the midpoints of shared edges, then
  string-pulled so creatures run straight wherever the ground allows. Connected
  components are precomputed, so an impossible path returns at once. About 0.2 ms per
  path on Lookout Point.
- `snap(p)`, `nearestCell(p)`: put spawns and knocked-back creatures back on the ground.
- `randomPointNear(p, r)`: wander targets, only ever on reachable ground.
- `exits()`: cells next to region exits, with the `.tng` UIDs.

Not handled yet: the z-level on leaves (bridges over walkable ground use it) and doors
that open and close (the interactive points).
