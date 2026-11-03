#!/usr/bin/env node
// Reads the region list from YOUR Fable install and writes shared/defs/regions.json.
//   node tools/extractor/regions.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// Sources (both plain text in the retail game):
//   data/Levels/FinalAlbion.wld                  one NewMap ... EndMap block per region
//   data/Misc/FinalAlbion_StartingRegionGraph.txt which regions border which
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
console.log(`${maps.length} maps, ${Object.keys(graph).length} regions in the graph -> ${out}`);
