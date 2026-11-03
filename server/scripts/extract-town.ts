// Reads villages, shops and their stock from your own Fable install and writes server/data/town-data.json.
//   npm run extract-town -- "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// The output stays on the server machine. Never commit it.
import fs from 'node:fs';
import path from 'node:path';
import { findDefs } from '../src/data/extract.js';
import { extractTown } from '../src/data/town-extract.js';
import { readTngFiles } from '../src/data/wad.js';

const dir = process.argv[2];
const out = process.argv[3] ?? 'data/town-data.json';
if (!dir) {
  console.error('usage: npm run extract-town -- "<Fable install folder>" [out.json]');
  process.exit(2);
}
const find = (folder: string, want: string) => {
  const hit = fs.existsSync(folder) && fs.readdirSync(folder).find((f) => f.toLowerCase() === want.toLowerCase());
  return hit ? path.join(folder, hit) : undefined;
};
const levels = find(find(dir, 'data') ?? dir, 'Levels');
const wad = levels && find(levels, 'FinalAlbion.wad');
const wld = levels && find(levels, 'FinalAlbion.wld');
if (!wad || !wld) {
  console.error(`Could not find data/Levels/FinalAlbion.wad and FinalAlbion.wld under ${dir}`);
  process.exit(1);
}
const { game, names } = findDefs(dir);
const town = extractTown(fs.readFileSync(game), fs.readFileSync(names), readTngFiles(wad), fs.readFileSync(wld, 'latin1'));
fs.mkdirSync(path.dirname(path.resolve(out)), { recursive: true });
fs.writeFileSync(out, JSON.stringify(town, null, 1));
const regions = Object.values(town.regions);
const sum = (f: (r: (typeof regions)[number]) => number) => regions.reduce((s, r) => s + f(r), 0);
console.log(
  `wrote ${out}: ${Object.keys(town.shopTypes).length} shop types, ${Object.keys(town.villages).length} village types, ` +
    `${sum((r) => r.shops.length)} shops, ${sum((r) => r.homes.length)} homes, ${sum((r) => r.villagers.length)} placed villagers in ${regions.length} regions`,
);
