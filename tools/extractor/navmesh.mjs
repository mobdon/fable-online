#!/usr/bin/env node
// Reads the walkable ground of every region from YOUR Fable install and writes
// shared/defs/nav/<LevelScriptName>.json (one level each, level frame) and
// shared/defs/nav-regions/<Region>.json (every level of a server region joined into the
// region's frame, the frame spawns and things use) for the server's pathfinding.
//   node tools/extractor/navmesh.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
//   node tools/extractor/navmesh.mjs <install> --only LookoutPoint,PicnicArea
//
// Sources (retail game, nothing from them is committed):
//   data/Levels/FinalAlbion.wad  "BBBB" archive holding every <Level>.lev and <Level>.tng
//   data/Levels/FinalAlbion.wld  which .lev belongs to which region script, and where it sits in the world
//
// What a .lev gives us (format notes in docs/navigation.md):
//   - heightmap: (width+1) x (height+1) samples, 1 m apart, height = value * 2048 metres
//   - navigation: a quadtree per subset. Leaves are walkable squares from 32 m down to 0.5 m,
//     each with the ids of the leaves it touches. Exit leaves also carry the UIDs of the
//     region-exit things in the .tng next to them.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseWld, regionFrames } from './things.mjs';

const isMain = !!process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url));
// Only read the command line when run as a script, not when another tool or a test imports this.
const args = isMain ? process.argv.slice(2) : [];
const game = args.find((a) => !a.startsWith('--'));
const onlyArg = args.indexOf('--only');
const only = onlyArg >= 0 ? new Set(args[onlyArg + 1].split(',')) : null;
if (!game && isMain) {
  console.error('usage: navmesh.mjs <Fable install folder> [--only Script1,Script2]');
  process.exit(1);
}
const levels = game ? path.join(game, 'data', 'Levels') : '';
const here = path.dirname(fileURLToPath(import.meta.url));
const outDir = path.join(here, '..', '..', 'shared', 'defs', 'nav');
const regionDir = path.join(here, '..', '..', 'shared', 'defs', 'nav-regions');

/** Real thing UID = stored value + this (the .lev stores the low part only). */
export const UID_BASE = 18446741874686296064n;

// ---------- FinalAlbion.wld: level file -> script name + world offset ----------
const wld = game ? fs.readFileSync(path.join(levels, 'FinalAlbion.wld'), 'latin1') : '';
const maps = new Map(); // lower-case level file name -> map info
for (const block of wld.split(/\bNewMap\s+/).slice(1)) {
  const body = block.split(/\bEndMap\b/)[0];
  const get = (key) => body.match(new RegExp(`\\b${key}\\s+([^;]+);`))?.[1].trim().replace(/^"|"$/g, '');
  const level = get('LevelName');
  if (!level) continue;
  const file = level.split('\\').pop().toLowerCase();
  maps.set(file, { script: get('LevelScriptName'), mapX: Number(get('MapX')), mapY: Number(get('MapY')), uid: Number(get('MapUID')) });
}

// ---------- FinalAlbion.wad ----------
export function readWadIndex(fd) {
  const head = Buffer.alloc(32);
  fs.readSync(fd, head, 0, 32, 0);
  if (head.toString('latin1', 0, 4) !== 'BBBB') throw new Error('not a BBBB archive');
  const count = head.readUInt32LE(20);
  const tableAt = head.readUInt32LE(28);
  const size = fs.fstatSync(fd).size - tableAt;
  const t = Buffer.alloc(size);
  fs.readSync(fd, t, 0, size, tableAt);
  const files = [];
  let o = 12;
  for (let i = 0; i < count; i++) {
    const fileSize = t.readUInt32LE(o + 12);
    const offset = t.readUInt32LE(o + 16);
    const nameLen = t.readUInt32LE(o + 24);
    o += 28;
    const name = t.toString('latin1', o, o + nameLen);
    o += nameLen;
    const extra = t.readUInt32LE(o + 12); // timestamps block
    o += 16 + extra;
    files.push({ name, offset, size: fileSize });
  }
  return files;
}

// ---------- .lev ----------
export function parseLev(d) {
  const navOffset = d.readUInt32LE(21);
  const width = d.readUInt32LE(38);
  const height = d.readUInt32LE(42);

  // Heightmap starts right after the sound theme names; the last one is followed by the
  // first cell, whose size field is always 21.
  let o = findHeightmap(d);
  const n = (width + 1) * (height + 1);
  const heights = new Float32Array(n);
  for (let i = 0; i < n; i++) {
    const size = d.readUInt32LE(o);
    if (size !== 21) throw new Error(`heightmap cell ${i}: size ${size}`);
    heights[i] = d.readFloatLE(o + 5) * 2048;
    o += size;
  }

  // Navigation header: sectionsStart, count, then (name, start) per section.
  const sectionCount = d.readUInt32LE(navOffset + 4);
  o = navOffset + 8;
  const sections = [];
  for (let i = 0; i < sectionCount; i++) {
    const len = d.readUInt32LE(o);
    const name = d.toString('latin1', o + 4, o + 4 + len);
    o += 4 + len;
    sections.push({ name, start: d.readUInt32LE(o) });
    o += 4;
  }

  const out = [];
  for (const sec of sections) {
    o = sec.start;
    o += 4 * 4; // size, version, width (f32), height (f32)
    o += 4; // number of quadtree layers
    const interactive = d.readUInt32LE(o);
    o += 4 + interactive * 12;
    const subsets = d.readUInt32LE(o);
    const nodeCount = d.readUInt32LE(o + 4);
    o += 8;
    const cells = [];
    for (let i = 0; i < nodeCount; i++) {
      const isExit = d[o];
      const blank = d[o + 2];
      o += 3;
      if (blank && !isExit) continue; // 32 m root block with no walkable ground at all
      const end = d[o];
      const layer = d[o + 1];
      const subset = d[o + 2];
      o += 3;
      const x = d.readFloatLE(o);
      const y = d.readFloatLE(o + 4);
      const id = d.readUInt32LE(o + 8);
      o += 12;
      if (!end) {
        o += 16; // inner node: four child ids
        continue;
      }
      const zLevel = d.readUInt32LE(o);
      const adjCount = d.readUInt32LE(o + 5);
      o += 9;
      const adj = [];
      for (let k = 0; k < adjCount; k++) adj.push(d.readUInt32LE(o + 4 * k));
      o += 4 * adjCount;
      const cell = { id, x, y, size: 32 / 2 ** layer, subset, z: zLevel, adj };
      if (isExit) {
        const uidCount = d.readUInt32LE(o);
        o += 4;
        cell.exit = [];
        for (let k = 0; k < uidCount; k++) cell.exit.push((d.readBigUInt64LE(o + 8 * k) + UID_BASE).toString());
        o += 8 * uidCount;
      }
      cells.push(cell);
    }
    out.push({ name: sec.name, subsets, cells, end: o });
  }
  return { width, height, heights, sections: out };
}

function findHeightmap(d) {
  // Sound theme names are length-prefixed "SOUND_THEME_*" strings. Skip past the last one.
  const s = d.toString('latin1', 0, Math.min(d.length, 200_000));
  let last = -1;
  for (const m of s.matchAll(/SOUND_THEME_[A-Z0-9_]*/g)) last = m.index + m[0].length;
  if (last < 0) throw new Error('no sound themes found');
  return last;
}

// ---------- main ----------
function main() {
  const fd = fs.openSync(path.join(levels, 'FinalAlbion.wad'), 'r');
  const files = readWadIndex(fd).filter((f) => f.name.toLowerCase().endsWith('.lev'));
  fs.mkdirSync(outDir, { recursive: true });
  let written = 0;
  let cellsTotal = 0;
  const skipped = [];
  let empty = 0;
  const byScript = new Map();
  for (const f of files) {
    const file = f.name.split('\\').pop();
    const map = maps.get(file.toLowerCase());
    const script = map?.script ?? file.replace(/\.lev$/i, '');
    if (only && !only.has(script)) continue;
    const buf = Buffer.alloc(f.size);
    fs.readSync(fd, buf, 0, f.size, f.offset);
    let lev;
    try {
      lev = parseLev(buf);
    } catch (e) {
      skipped.push(`${script}: ${e.message}`);
      continue;
    }
    const cells = lev.sections.flatMap((s) => s.cells);
    if (cells.length === 0) {
      empty++; // sea, filler and backdrop levels have no ground to stand on
      continue;
    }
    const doc = {
      v: 1,
      source: f.name,
      script,
      mapX: map?.mapX ?? 0,
      mapY: map?.mapY ?? 0,
      width: lev.width,
      height: lev.height,
      heights: Buffer.from(lev.heights.buffer).toString('base64'),
      // [id, x, y, size, subset, zLevel, [adjacent ids]]
      cells: cells.map((c) => [c.id, c.x, c.y, c.size, c.subset, c.z, c.adj]),
      exits: cells.filter((c) => c.exit).map((c) => [c.id, c.exit]),
    };
    fs.writeFileSync(path.join(outDir, `${script}.json`), JSON.stringify(doc));
    byScript.set(script, doc);
    written++;
    cellsTotal += cells.length;
  }
  fs.closeSync(fd);

  // Server regions: each one's levels joined in the region's frame.
  const { levelInfo } = regionFrames(parseWld(wld));
  const parts = new Map(); // region -> [{ doc, dx, dy }]
  for (const [script, doc] of byScript) {
    const L = levelInfo[script];
    if (!L) continue;
    if (!parts.has(L.region)) parts.set(L.region, []);
    parts.get(L.region).push({ doc, dx: L.dx, dy: L.dy });
  }
  fs.mkdirSync(regionDir, { recursive: true });
  let seams = 0;
  for (const [region, list] of parts) {
    const merged = mergeLevels(region, list);
    seams += merged.seams;
    delete merged.seams;
    fs.writeFileSync(path.join(regionDir, `${region}.json`), JSON.stringify(merged));
  }
  console.log(`${parts.size} server regions -> ${regionDir} (${seams} links across level seams)`);
  console.log(`${written} regions, ${cellsTotal} walkable cells -> ${outDir} (${empty} backdrop levels with no ground)`);
  if (skipped.length) {
    console.log(`failed ${skipped.length}:\n  ${skipped.join('\n  ')}`);
    process.exitCode = 1;
  }
}

/**
 * Join a region's levels into one nav file in the region's frame. Each part is a per-level
 * doc plus its offset (dx, dy) from the region's anchor level. The result carries originX/Y,
 * the region-frame position of its heightmap's corner (a level west or south of the anchor
 * gives a negative origin). Cells get new ids; cells of different levels that share an edge
 * are linked, so creatures can walk across level seams.
 */
export function mergeLevels(region, list) {
  const ox = Math.min(...list.map((p) => p.dx));
  const oy = Math.min(...list.map((p) => p.dy));
  const W = Math.max(...list.map((p) => p.dx + p.doc.width)) - ox;
  const H = Math.max(...list.map((p) => p.dy + p.doc.height)) - oy;
  const heights = new Float32Array((W + 1) * (H + 1));
  const covered = new Uint8Array((W + 1) * (H + 1));
  const cells = [];
  const exits = [];
  const owner = []; // cell index -> part index
  for (let pi = 0; pi < list.length; pi++) {
    const { doc, dx, dy } = list[pi];
    const raw = Buffer.from(doc.heights, 'base64');
    const h = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
    const row = doc.width + 1;
    for (let y = 0; y <= doc.height; y++)
      for (let x = 0; x <= doc.width; x++) {
        const i = (y + dy - oy) * (W + 1) + (x + dx - ox);
        heights[i] = h[y * row + x];
        covered[i] = 1;
      }
    const ids = new Map();
    for (const c of doc.cells) ids.set(c[0], cells.length + ids.size);
    for (const c of doc.cells) {
      owner.push(pi);
      cells.push([ids.get(c[0]), c[1] + dx, c[2] + dy, c[3], c[4], c[5], c[6].map((a) => ids.get(a)).filter((a) => a !== undefined)]);
    }
    for (const [id, uids] of doc.exits ?? []) if (ids.has(id)) exits.push([ids.get(id), uids]);
  }
  // Fill gaps between levels with the nearest covered sample on the row (nothing walks there).
  for (let y = 0; y <= H; y++) {
    let last = NaN;
    for (let x = 0; x <= W; x++) {
      const i = y * (W + 1) + x;
      if (covered[i]) last = heights[i];
      else if (!Number.isNaN(last)) heights[i] = last;
    }
  }
  // Seams: bucket cells by 1 m square, then link touching cells of different levels.
  let seams = 0;
  if (list.length > 1) {
    const buckets = new Map();
    const key = (x, y) => y * (W + 2) + x;
    cells.forEach((c, i) => {
      const half = c[3] / 2;
      for (let y = Math.floor(c[2] - half - oy) - 1; y <= Math.floor(c[2] + half - oy) + 1; y++)
        for (let x = Math.floor(c[1] - half - ox) - 1; x <= Math.floor(c[1] + half - ox) + 1; x++) {
          const k = key(x, y);
          if (!buckets.has(k)) buckets.set(k, []);
          buckets.get(k).push(i);
        }
    });
    const linked = new Set();
    for (const b of buckets.values())
      for (let i = 0; i < b.length; i++)
        for (let j = i + 1; j < b.length; j++) {
          const a = b[i], c = b[j];
          if (owner[a] === owner[c]) continue;
          const pair = a < c ? `${a}:${c}` : `${c}:${a}`;
          if (linked.has(pair) || !touching(cells[a], cells[c])) continue;
          linked.add(pair);
          cells[a][6].push(cells[c][0]);
          cells[c][6].push(cells[a][0]);
          seams++;
        }
  }
  return {
    v: 1,
    script: region,
    region: true,
    mapX: 0,
    mapY: 0,
    originX: ox,
    originY: oy,
    width: W,
    height: H,
    heights: Buffer.from(heights.buffer).toString('base64'),
    cells,
    exits,
    seams,
  };
}

/**
 * Cells of two levels meet: they share a stretch of edge or overlap, allowing a small gap
 * where the levels' edges don't quite line up. Corners alone don't count.
 */
export function touching(a, b) {
  const ha = a[3] / 2, hb = b[3] / 2;
  const gapX = Math.abs(a[1] - b[1]) - (ha + hb);
  const gapY = Math.abs(a[2] - b[2]) - (ha + hb);
  const SEAM_GAP = 0.6;
  return gapX <= SEAM_GAP && gapY <= SEAM_GAP && (gapX < -0.05 || gapY < -0.05);
}

if (isMain) main();
