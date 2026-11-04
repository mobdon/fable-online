#!/usr/bin/env node
// Reads the list of quest names from YOUR Fable install and writes shared/defs/quests.json.
//   node tools/extractor/quests.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// Source: data/Levels/FinalAlbion.qst and GlobalQuests.qst (plain text, AddQuest("Name", TRUE|FALSE);).
// The server pays a quest reward only for quests in this list (or with an explicit rule).
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export function parseQst(text) {
  return [...text.matchAll(/AddQuest\(\s*"([^"]+)"/g)].map((m) => m[1]);
}

const game = process.argv[2];
if (!game) {
  console.error('usage: quests.mjs <Fable install folder>');
  process.exit(1);
}
const levels = path.join(game, 'data', 'Levels');
const quests = new Set();
for (const f of ['FinalAlbion.qst', 'GlobalQuests.qst']) {
  const p = path.join(levels, f);
  if (fs.existsSync(p)) for (const q of parseQst(fs.readFileSync(p, 'latin1'))) quests.add(q);
}
const here = path.dirname(fileURLToPath(import.meta.url));
const out = path.join(here, '..', '..', 'shared', 'defs', 'quests.json');
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, JSON.stringify({ source: 'FinalAlbion.qst', quests: [...quests].sort() }, null, 1));
console.log(`${quests.size} quests -> ${out}`);
