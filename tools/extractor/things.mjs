#!/usr/bin/env node
// Reads the interactive things of every level from YOUR Fable install and writes
// shared/defs/world-things.json for the server: region exits and the entrances they
// lead to, doors, chests, searchable containers, items lying in the world, digging spots,
// demon doors, doors that teleport and Cullis Gates.
//   node tools/extractor/things.mjs "C:\Program Files (x86)\Steam\steamapps\common\Fable The Lost Chapters"
//
// Sources (retail game, nothing from them is committed):
//   data/Levels/FinalAlbion.wad  every <Level>.tng (plain-text thing lists) inside a BBBB archive
//   data/Levels/FinalAlbion.gtg  global things, incl. the region entrance points, one NEWMAP block per map
//   data/Levels/FinalAlbion.wld  maps (NewMap: id, MapX/MapY, script) and regions (NewRegion: ContainsMap)
//
// Format notes are in docs/world-objects.md.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

// ---------------------------------------------------------------- archive

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
    const extra = t.readUInt32LE(o + 12);
    o += 16 + extra;
    files.push({ name, offset, size: fileSize });
  }
  return files;
}

// ---------------------------------------------------------------- .wld

export function parseWld(text) {
  const maps = new Map(); // id -> { id, script, level, mapX, mapY }
  for (const block of text.split(/\bNewMap\s+/).slice(1)) {
    const body = block.split(/\bEndMap\b/)[0];
    const get = (key) => body.match(new RegExp(`\\b${key}\\s+([^;]+);`))?.[1].trim().replace(/^"|"$/g, '');
    const id = parseInt(body, 10);
    const level = get('LevelName');
    maps.set(id, { id, uid: Number(get('MapUID')), script: get('LevelScriptName'), level: level?.split('\\').pop().toLowerCase(), mapX: Number(get('MapX')), mapY: Number(get('MapY')) });
  }
  const regions = [];
  for (const block of text.split(/\bNewRegion\s+/).slice(1)) {
    const body = block.split(/\bEndRegion\b/)[0];
    const name = body.match(/\bRegionName\s+"([^"]+)"/)?.[1];
    const contains = [...body.matchAll(/\bContainsMap\s+"([^"]+)"/g)].map((m) => m[1].split('\\').pop().toLowerCase());
    if (name) regions.push({ name, contains });
  }
  return { maps, regions };
}

// ---------------------------------------------------------------- .tng / .gtg

/**
 * Generic thing parser. Returns { kind, uid, def, script, section, pos, tc: { CTCName: { key: value } }, top: { key: value } }.
 * Values are kept as strings; callers convert what they need.
 */
export function parseThings(text) {
  const out = [];
  let section = 'NULL';
  let cur = null;
  let tc = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (!line) continue;
    let m;
    if ((m = line.match(/^XXXSectionStart\s+([^;]+);/))) {
      section = m[1].trim();
      continue;
    }
    if (line.startsWith('XXXSectionEnd')) {
      section = 'NULL';
      continue;
    }
    if ((m = line.match(/^NewThing\s+([^;]+);/))) {
      cur = { kind: m[1].trim(), section, top: {}, tc: {} };
      tc = null;
      continue;
    }
    if (!cur) continue;
    if (line === 'EndThing;') {
      cur.uid = cur.top.UID;
      cur.def = (cur.top.DefinitionType ?? '').replace(/^"|"$/g, '');
      cur.script = cur.top.ScriptName && cur.top.ScriptName !== 'NULL' ? cur.top.ScriptName : null;
      const p = cur.tc.CTCPhysicsStandard ?? cur.tc.CTCPhysicsNavigator ?? cur.tc.CTCPhysicsLight;
      cur.pos = p ? [num(p.PositionX), num(p.PositionY), num(p.PositionZ)] : null;
      if (p && p.RHSetForwardX !== undefined) cur.fwd = [num(p.RHSetForwardX), num(p.RHSetForwardY)];
      out.push(cur);
      cur = null;
      continue;
    }
    if ((m = line.match(/^Start(CTC[A-Za-z]+);/))) {
      tc = cur.tc[m[1]] = {};
      continue;
    }
    if (/^End(CTC[A-Za-z]+);/.test(line)) {
      tc = null;
      continue;
    }
    if ((m = line.match(/^([A-Za-z_]+(?:\[[^\]]*\])?)\s+(.*?);$/))) (tc ?? cur.top)[m[1]] = m[2].trim();
  }
  return out;
}

/** Stable id for a thing: UIDs repeat between levels, so the level is part of it. */
const thingId = (level, uid) => `${level}:${low(uid)}`;
const num = (s) => Math.round(Number(s) * 1000) / 1000;
const str = (s) => (s ?? '').replace(/^"|"$/g, '');
const bool = (s) => s === 'TRUE';
const LOW32 = 0xffffffffn;
const low = (uid) => Number(BigInt(uid) & LOW32);
/**
 * Thing UIDs are only unique inside one map. A link to a thing in another map
 * (EntranceConnectedToUID) packs the target map's MapUID into the high bits:
 * (MapUID << 40) | uid-within-map.
 */
const linkKey = (uid) => {
  const v = BigInt(uid);
  return `${Number(v >> 40n)}:${low(v)}`;
};
const contents = (c) =>
  Object.entries(c ?? {})
    .filter(([k]) => k.startsWith('ContainerContents['))
    .map(([, v]) => str(v))
    .filter(Boolean);

// ---------------------------------------------------------------- build

/**
 * Which region each level belongs to and how far its frame is from the region's frame.
 * A region's frame is its anchor level's: the contained level whose script matches the
 * region name, else the first contained level in .wld order. That is the same rule as the
 * server's RegionTable.origin() (server/src/world/regions.ts), which reads regions.json in
 * .wld order, so positions converted here line up with the world coordinates Fable reports.
 * Returns { levelInfo: script -> { region, mapUid, mapX, mapY, dx, dy }, regionAnchor: region -> script }.
 */
export function regionFrames(wld) {
  const byLevelFile = new Map([...wld.maps.values()].map((m) => [m.level, m]));
  const order = new Map([...wld.maps.values()].map((m, i) => [m, i]));
  const levelInfo = {};
  const regionAnchor = {};
  for (const r of wld.regions) {
    const maps = r.contains.map((f) => byLevelFile.get(f)).filter(Boolean);
    if (!maps.length) continue;
    const anchor = maps.find((m) => m.script === r.name) ?? [...maps].sort((a, b) => order.get(a) - order.get(b))[0];
    regionAnchor[r.name] = anchor.script;
    for (const m of maps) {
      if (levelInfo[m.script]) continue; // a level belongs to the first region that contains it
      levelInfo[m.script] = { region: r.name, mapUid: m.uid, mapX: m.mapX, mapY: m.mapY, dx: m.mapX - anchor.mapX, dy: m.mapY - anchor.mapY };
    }
  }
  return { levelInfo, regionAnchor };
}

/**
 * Turn parsed levels into the server's table. `levels` is [{ script, things }], `entrances`
 * are the region entrance things from the .gtg with their map id already resolved to a
 * level script. Positions are converted into each region's frame: the frame of the region's
 * anchor level (the contained level whose script matches the region name, else the first).
 */
export function build(wld, levels, gtgEntrances) {
  const byScript = new Map([...wld.maps.values()].map((m) => [m.script, m]));
  const { levelInfo, regionAnchor } = regionFrames(wld);
  const toRegion = (level, pos) => {
    const L = levelInfo[level];
    return pos && L ? [num(pos[0] + L.dx), num(pos[1] + L.dy), pos[2]] : pos;
  };

  const out = { v: 1, levels: levelInfo, regionAnchor, exits: [], entrances: [], doors: [], chests: [], containers: [], pickups: [], digSpots: [], demonDoors: [], hooks: [], gates: [] };
  const entranceByKey = new Map();
  const addEntrance = (level, t) => {
    const key = `${byScript.get(level)?.uid}:${low(t.uid)}`;
    if (entranceByKey.has(key)) return;
    const L = levelInfo[level];
    const e = { id: thingId(level, t.uid), uid: t.uid, level, region: L?.region ?? null, pos: toRegion(level, t.pos), fwd: t.fwd ?? null };
    entranceByKey.set(key, e);
    out.entrances.push(e);
  };
  for (const { script, things } of levels) for (const t of things) if (t.tc.CTCDRegionEntrance) addEntrance(script, t);
  for (const { level, thing } of gtgEntrances) addEntrance(level, thing);

  for (const { script: level, things } of levels) {
    const L = levelInfo[level];
    const region = L?.region ?? null;
    const base = (t) => ({ id: thingId(level, t.uid), uid: t.uid, def: t.def, level, region, section: t.section, script: t.script, pos: toRegion(level, t.pos) });
    for (const t of things) {
      if (!t.pos) continue;
      if (t.tc.CTCDRegionExit) {
        const x = t.tc.CTCDRegionExit;
        const target = entranceByKey.get(linkKey(x.EntranceConnectedToUID ?? '0'));
        out.exits.push({
          ...base(t),
          active: bool(x.Active),
          radius: Number(x.Radius ?? 3),
          to: target ? { entrance: target.id, region: target.region, level: target.level, pos: target.pos, fwd: target.fwd } : null,
        });
      }
      if (t.tc.CTCDoor) {
        const d = t.tc.CTCDoor;
        out.doors.push({ ...base(t), open: bool(d.Open), trigger: Number(d.DoorTriggerType ?? 0), owner: t.tc.CTCOwnedEntity?.OwnerUID ?? null, prison: /PRISON|JAIL/.test(t.def) });
      }
      if (t.tc.CTCChest) {
        const keys = t.def.match(/^OBJECT_SILVERKEY_CHEST_(\d+)/);
        out.chests.push({ ...base(t), contents: contents(t.tc.CTCChest), open: bool(t.tc.CTCChest.ChestOpen), silverKeys: keys ? Number(keys[1]) : 0, persistent: bool(t.top.ThingGamePersistent) });
      }
      if (t.tc.CTCSearchableContainer) out.containers.push({ ...base(t), contents: contents(t.tc.CTCContainerRewardHero) });
      if (t.tc.CTCDiggingSpot) out.digSpots.push({ ...base(t), contents: contents(t.tc.CTCContainerRewardHero), hidden: bool(t.tc.CTCDiggingSpot.Hidden) });
      if (t.tc.CTCInventoryItem) {
        const s = t.tc.CTCStockItem;
        out.pickups.push({ ...base(t), forSale: s ? bool(s.ForSale) : false, stealable: s ? bool(s.Stealable) : false, price: s ? Number(s.Price ?? 0) : 0, persistent: bool(t.top.ThingGamePersistent) });
      }
      if (/^CREATURE_DEMON_DOOR_FACE/.test(t.def) || t.script === 'DemonDoorFace') out.demonDoors.push({ ...base(t), exit: null });
      const gate = gateKind(t);
      if (gate) out.gates.push({ ...base(t), fwd: t.fwd ?? null, guild: gate === 'guild' });
      const h = t.tc.CTCActionUseScriptedHook;
      if (h && bool(h.TeleportToRegionEntrance)) {
        const target = entranceByKey.get(linkKey(h.EntranceConnectedToUID ?? '0'));
        out.hooks.push({ ...base(t), to: target ? { entrance: target.id, region: target.region, level: target.level, pos: target.pos, fwd: target.fwd } : null });
      }
    }
  }

  // The same gate can sit in a level twice (an "off" and an "on" model swapped by a quest
  // section). Keep one per spot, preferring the one that always exists.
  out.gates.sort((a, b) => (a.section === 'NULL' ? 0 : 1) - (b.section === 'NULL' ? 0 : 1));
  out.gates = out.gates.filter((g, i) => !out.gates.slice(0, i).some((o) => o.region === g.region && (o.pos[0] - g.pos[0]) ** 2 + (o.pos[1] - g.pos[1]) ** 2 < 10 * 10));

  // A demon door guards the region exit next to it (the way into its DemonDoor_* region).
  for (const dd of out.demonDoors) {
    let best = null;
    let bestD = 20 * 20;
    for (const e of out.exits) {
      if (e.region !== dd.region || !e.to) continue;
      const d = (e.pos[0] - dd.pos[0]) ** 2 + (e.pos[1] - dd.pos[1]) ** 2;
      if (d < bestD) {
        bestD = d;
        best = e;
      }
    }
    dd.exit = best?.id ?? null;
  }
  return out;
}

/**
 * Cullis Gates are things with Fable's CTCTeleporter component; the Guild's pedestal is one
 * too. The def names are a fallback in case a gate model carries the component under
 * another name. Teleporters inside buyable houses belong to the property system, not here.
 */
const GATE_DEFS = /TELEPORTER_GRIFFONS|HKC_TELEPORTER_ON|GUILD_PEDESTAL_TELEPORT/;
export function gateKind(t) {
  if (/BUYABLE_HOUSE|_OFF_/.test(t.def)) return null;
  if (!t.tc.CTCTeleporter && !GATE_DEFS.test(t.def)) return null;
  return /GUILD_PEDESTAL/.test(t.def) ? 'guild' : 'gate';
}

// ---------------------------------------------------------------- main

function main() {
  const game = process.argv[2];
  if (!game) {
    console.error('usage: things.mjs <Fable install folder>');
    process.exit(1);
  }
  const dir = path.join(game, 'data', 'Levels');
  const wld = parseWld(fs.readFileSync(path.join(dir, 'FinalAlbion.wld'), 'latin1'));
  const byFile = new Map([...wld.maps.values()].map((m) => [m.level, m]));

  const fd = fs.openSync(path.join(dir, 'FinalAlbion.wad'), 'r');
  const levels = [];
  for (const f of readWadIndex(fd)) {
    if (!f.name.toLowerCase().endsWith('.tng')) continue;
    const levFile = f.name.split('\\').pop().toLowerCase().replace(/\.tng$/, '.lev');
    const map = byFile.get(levFile);
    if (!map?.script) continue;
    const buf = Buffer.alloc(f.size);
    fs.readSync(fd, buf, 0, f.size, f.offset);
    levels.push({ script: map.script, things: parseThings(buf.toString('latin1')) });
  }
  fs.closeSync(fd);

  // .gtg: NEWMAP <map id> blocks, each holding that map's global things.
  const gtg = fs.readFileSync(path.join(dir, 'FinalAlbion.gtg'), 'latin1');
  const gtgEntrances = [];
  for (const block of gtg.split(/^NEWMAP\s+/m).slice(1)) {
    const level = wld.maps.get(parseInt(block, 10))?.script;
    if (!level) continue;
    for (const t of parseThings(block)) if (t.tc.CTCDRegionEntrance && t.pos) gtgEntrances.push({ level, thing: t });
  }

  const doc = build(wld, levels, gtgEntrances);
  const here = path.dirname(fileURLToPath(import.meta.url));
  const out = path.join(here, '..', '..', 'shared', 'defs', 'world-things.json');
  fs.mkdirSync(path.dirname(out), { recursive: true });
  fs.writeFileSync(out, JSON.stringify(doc));
  const linked = doc.exits.filter((e) => e.to).length;
  console.log(
    `${levels.length} levels: ${doc.exits.length} exits (${linked} linked), ${doc.entrances.length} entrances, ${doc.doors.length} doors, ` +
      `${doc.chests.length} chests, ${doc.containers.length} containers, ${doc.pickups.length} pickups, ${doc.digSpots.length} digging spots, ` +
      `${doc.demonDoors.length} demon doors, ${doc.hooks.length} teleport doors, ${doc.gates.length} Cullis Gates -> ${out}`,
  );
}

if (process.argv[1] && path.resolve(process.argv[1]) === path.resolve(fileURLToPath(import.meta.url))) main();
