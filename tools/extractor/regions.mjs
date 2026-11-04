#!/usr/bin/env node
// Reads the region list from YOUR Fable install and writes shared/defs/regions.json.
//   node tools/extractor/regions.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// Sources (both plain text in the retail game):
//   data/Levels/FinalAlbion.wld                  one NewMap ... EndMap block per region
//   data/Misc/FinalAlbion_StartingRegionGraph.txt which regions border which
//   data/Levels/FinalAlbion.wad                  each .lev's width and height (header only),
//                                                so the server can tell which region a world
//                                                position is in
import fs from 'node:fs';
import path from 'node:path';

const game = process.argv[2];
if (!game) { console.error('usage: regions.mjs <Fable install folder>'); process.exit(1); }
const read = (...p) => fs.readFileSync(path.join(game, ...p), 'latin1');

const wld = read('data', 'Levels', 'FinalAlbion.wld');
const maps = [];
for (const block of wld.split(/\bNewMap\s+/).slice(1)) {
  const body = block.split(/\bEndMap\b/)[0];
  const id = parseInt(body, 10);
  const get = (key) => body.match(new RegExp(`\\b${key}\\s+([^;]+);`))?.[1].trim();
  const str = (key) => get(key)?.replace(/^"|"$/g, '');
  maps.push({
    id,
    uid: Number(get('MapUID')),
    script: str('LevelScriptName'),
    level: str('LevelName'),
    x: Number(get('MapX')),
    y: Number(get('MapY')),
    sea: get('IsSea') === 'TRUE',
    loadedOnProximity: get('LoadedOnPlayerProximity') === 'TRUE',
  });
}

// Level sizes from the .lev headers in the WAD (a BBBB archive). Same layout as
// navmesh.mjs reads; only the 46-byte header of each level is touched.
const sizes = new Map(); // lower-case .lev file name -> [width, height] in metres
try {
  const fd = fs.openSync(path.join(game, 'data', 'Levels', 'FinalAlbion.wad'), 'r');
  const head = Buffer.alloc(32);
  fs.readSync(fd, head, 0, 32, 0);
  if (head.toString('latin1', 0, 4) !== 'BBBB') throw new Error('not a BBBB archive');
  const count = head.readUInt32LE(20);
  const tableAt = head.readUInt32LE(28);
  const t = Buffer.alloc(fs.fstatSync(fd).size - tableAt);
  fs.readSync(fd, t, 0, t.length, tableAt);
  let o = 12;
  const lev = Buffer.alloc(46);
  for (let i = 0; i < count; i++) {
    const fileSize = t.readUInt32LE(o + 12);
    const offset = t.readUInt32LE(o + 16);
    const nameLen = t.readUInt32LE(o + 24);
    o += 28;
    const name = t.toString('latin1', o, o + nameLen);
    o += nameLen;
    o += 16 + t.readUInt32LE(o + 12);
    if (!/\.lev$/i.test(name) || fileSize < 46) continue;
    fs.readSync(fd, lev, 0, 46, offset);
    sizes.set(name.split('\\').pop().toLowerCase(), [lev.readUInt32LE(38), lev.readUInt32LE(42)]);
  }
  fs.closeSync(fd);
} catch (e) {
  console.warn(`level sizes not read (${e.message}); the server will not follow regions by position`);
}
for (const m of maps) {
  const sz = m.level && sizes.get(m.level.split('\\').pop().toLowerCase());
  if (sz && sz[0] > 0 && sz[0] <= 4096 && sz[1] > 0 && sz[1] <= 4096) [m.w, m.h] = sz;
}

// Which region (NewRegion: RegionName, the names the region graph and the server use) each
// level belongs to, from its ContainsMap lines.
const regionOf = new Map(); // lower-case .lev file name -> region name
for (const block of wld.split(/\bNewRegion\s+/).slice(1)) {
  const body = block.split(/\bEndRegion\b/)[0];
  const name = body.match(/\bRegionName\s+"([^"]+)"/)?.[1];
  if (!name) continue;
  for (const m of body.matchAll(/\bContainsMap\s+"([^"]+)"/g)) regionOf.set(m[1].split('\\').pop().toLowerCase(), name);
}
for (const m of maps) {
  const r = m.level && regionOf.get(m.level.split('\\').pop().toLowerCase());
  if (r) m.region = r;
}

const graphText = read('data', 'Misc', 'FinalAlbion_StartingRegionGraph.txt');
const graph = {};
for (const line of graphText.split(/\r?\n/)) {
  const m = line.match(/^\s*"([^"]+)"\s*:\s*(.*?);?\s*$/);
  if (!m) continue;
  graph[m[1]] = [...m[2].matchAll(/"([^"]+)"/g)].map((x) => x[1]);
}

const out = path.join(path.dirname(new URL(import.meta.url).pathname.replace(/^\/([A-Za-z]:)/, '$1')), '..', '..', 'shared', 'defs', 'regions.json');
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, JSON.stringify({ source: 'FinalAlbion.wld', maps, graph }, null, 2));
console.log(`${maps.length} maps (${maps.filter((m) => m.w).length} with sizes), ${Object.keys(graph).length} regions in the graph -> ${out}`);
