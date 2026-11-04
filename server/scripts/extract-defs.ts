// Reads game numbers from your own Fable install and writes server/data/game-data.json.
//   npm run extract -- "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// The output stays on the server machine. Never commit it.
import fs from 'node:fs';
import path from 'node:path';
import { extract, findDefs } from '../src/data/extract.js';

const dir = process.argv[2];
const out = process.argv[3] ?? 'data/game-data.json';
if (!dir) {
  console.error('usage: npm run extract -- "<Fable install folder>" [out.json]');
  process.exit(2);
}
const { game, names, script } = findDefs(dir);
const data = extract(fs.readFileSync(game), fs.readFileSync(names), script ? fs.readFileSync(script) : undefined);
fs.mkdirSync(path.dirname(path.resolve(out)), { recursive: true });
fs.writeFileSync(out, JSON.stringify(data, null, 1));
const weapons = Object.values(data.items).filter((i) => i.weapon).length;
console.log(
  `wrote ${out}: ${Object.keys(data.items).length} items (${weapons} weapons), ${Object.keys(data.creatures).length} creatures, ` +
    `${data.abilities.length} hero abilities, ${Object.keys(data.spells).length} spell defs, ` +
    `${Object.keys(data.questCards).length} quest cards (${Object.values(data.questCards).filter((c) => c.boastGold !== undefined).length} with boasts${script ? '' : '; no script.bin found'})`,
);
