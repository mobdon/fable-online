#!/usr/bin/env node
// Reads creature placements from YOUR Fable install and writes shared/defs/spawns.json.
//   node tools/extractor/spawns.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
// Sources:
//   data/Levels/FinalAlbion.wad   archive of .lev and .tng files ("BBBB" header)
//   data/Levels/FinalAlbion.wld   maps each level file to its LevelScriptName, and levels to regions
// Each .tng is plain text: NewThing <kind>; ... EndThing; blocks. We keep:
//   AICreature things            a creature placed in the level, with its def and position
//   MARKER_CREATURE_GENERATOR    a spawner that picks from CREATURE_GENERATION_FAMILY defs
// Spawns are grouped by region (the region graph's names, which the server's channels use) and
// positions are converted into the region's frame (its anchor level's; see regionFrames in
// things.mjs), the frame the server's level data and RegionTable.origin() share. Uids are
// prefixed with the level script so two levels of one region never clash.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseWld, regionFrames } from './things.mjs';

/** WAD index: 32-byte header, then entries at entriesOffset. */
export function readWadIndex(fd) {
  const head = Buffer.alloc(32);
  fs.readSync(fd, head, 0, 32, 0);
  if (head.toString('latin1', 0, 4) !== 'BBBB') throw new Error('not a Fable .wad');
  const count = head.readUInt32LE(20);
  const entriesOffset = head.readUInt32LE(28);
  const size = fs.fstatSync(fd).size - entriesOffset;
  const buf = Buffer.alloc(size);
  fs.readSync(fd, buf, 0, size, entriesOffset);
  const out = [];
  let o = 0;
  for (let i = 0; i < count; i++) {
    o += 16 + 8; // unknown[16], id, unknown
    const len = buf.readUInt32LE(o);
    const off = buf.readUInt32LE(o + 4);
    const pathLen = buf.readUInt32LE(o + 12);
    o += 16;
    const name = buf.toString('latin1', o, o + pathLen);
    o += pathLen + 16 + 28 + 28 + 20; // unknown[16], created, accessed, written
    out.push({ name, offset: off, size: len });
  }
  return out;
}

const num = (body, key) => {
  const m = body.match(new RegExp(`\\b${key}\\s+(-?[0-9.eE+-]+);`));
  return m ? Number(m[1]) : undefined;
};
const str = (body, key) => body.match(new RegExp(`\\b${key}\\s+"?([^";]*)"?;`))?.[1];

/** Parse one .tng text into the things the server cares about. */
export function parseTng(text) {
  const creatures = [];
  const generators = [];
  for (const m of text.matchAll(/NewThing (\w+);([\s\S]*?)EndThing;/g)) {
    const [, kind, body] = m;
    const def = str(body, 'DefinitionType');
    const pos = { x: num(body, 'PositionX') ?? 0, y: num(body, 'PositionY') ?? 0, z: num(body, 'PositionZ') ?? 0 };
    const fx = num(body, 'RHSetForwardX') ?? 0;
    const fy = num(body, 'RHSetForwardY') ?? 1;
    const yaw = Math.round(Math.atan2(-fx, fy) * 1000) / 1000 + 0; // + 0 turns -0 into 0
    const uid = str(body, 'UID');
    const script = str(body, 'ScriptName');
    if (kind === 'AICreature' && def) {
      creatures.push({ uid, def, ...pos, yaw, script: script === 'NULL' ? null : script });
    } else if (def === 'MARKER_CREATURE_GENERATOR') {
      const families = [...body.matchAll(/CreatureFamilies\[\d+\]\s+"([^"]+)"/g)].map((f) => f[1]);
      generators.push({
        uid,
        ...pos,
        families,
        radius: num(body, 'GenerationRadius') ?? 10,
        triggerRadius: num(body, 'SelfTriggerRadius') ?? 0,
        activeLimit: num(body, 'ActiveCreatureLimit') ?? -1,
        totalLimit: num(body, 'TotalGenerationLimit') ?? -1,
        script: script === 'NULL' ? null : script,
      });
    }
  }
  return { creatures, generators };
}

/**
 * Level tables -> region tables in each region's frame. `tables` is [{ script, creatures,
 * generators }] in level-local positions; `levelInfo` comes from regionFrames(). A level in
 * no region keeps its own script as the key and its own frame.
 */
export function groupByRegion(tables, levelInfo) {
  const regions = {};
  for (const t of tables) {
    if (!t.creatures.length && !t.generators.length) continue;
    const L = levelInfo[t.script];
    const dx = L?.dx ?? 0;
    const dy = L?.dy ?? 0;
    const move = (s) => ({ ...s, uid: `${t.script}:${s.uid}`, x: round(s.x + dx), y: round(s.y + dy) });
    const r = (regions[L?.region ?? t.script] ??= { creatures: [], generators: [] });
    r.creatures.push(...t.creatures.map(move));
    r.generators.push(...t.generators.map(move));
  }
  return regions;
}

const round = (n) => Math.round(n * 1000) / 1000;

function main() {
  const game = process.argv[2];
  if (!game) {
    console.error('usage: spawns.mjs <Fable install folder>');
    process.exit(1);
  }
  const levels = path.join(game, 'data', 'Levels');
  const wldText = fs.readFileSync(path.join(levels, 'FinalAlbion.wld'), 'latin1');
  const wld = parseWld(wldText);
  const { levelInfo } = regionFrames(wld);
  const scriptOf = new Map([...wld.maps.values()].filter((m) => m.level && m.script).map((m) => [path.basename(m.level, '.lev'), m.script]));
  const fd = fs.openSync(path.join(levels, 'FinalAlbion.wad'), 'r');
  const tables = [];
  for (const e of readWadIndex(fd)) {
    if (!e.name.toLowerCase().endsWith('.tng')) continue;
    const base = path.basename(e.name.replace(/\\/g, '/'), '.tng');
    const script = scriptOf.get(base.toLowerCase()) ?? base;
    const buf = Buffer.alloc(e.size);
    fs.readSync(fd, buf, 0, e.size, e.offset);
    tables.push({ script, ...parseTng(buf.toString('latin1')) });
  }
  const regions = groupByRegion(tables, levelInfo);
  let nc = 0;
  let ng = 0;
  for (const r of Object.values(regions)) {
    nc += r.creatures.length;
    ng += r.generators.length;
  }
  fs.closeSync(fd);
  const here = path.dirname(fileURLToPath(import.meta.url));
  const out = path.join(here, '..', '..', 'shared', 'defs', 'spawns.json');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, JSON.stringify({ source: 'FinalAlbion.wad', regions }, null, 1));
  console.log(`${nc} placed creatures, ${ng} generators in ${Object.keys(regions).length} regions -> ${out}`);
}

if (import.meta.url === `file://${process.argv[1]}` || process.argv[1]?.endsWith('spawns.mjs')) main();
