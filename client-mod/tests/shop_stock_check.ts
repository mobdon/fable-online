// Fixture and cross-check for the client's shop stock filter (src/game/shop_stock.cpp).
//
//   make <dir>   writes a small game.bin + names.bin in the retail layout (real zlib chunks)
//                and inflate test vectors
//   check <dir>  reads <dir>/filtered.bin (written by shop_stock_tests) with the server's own
//                game.bin reader and checks the banned rows are empty and the rest untouched
//
// Run from server/: npx tsx ../client-mod/tests/shop_stock_check.ts make /tmp/x
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import assert from 'node:assert/strict';
import { GameBin, NamesBin, fableHash } from '../../server/src/data/fable-bin.js';
import { extractShopTypes } from '../../server/src/data/town-extract.js';

const NONE = 0xffffffff;
const ARMOUR_FIELD = 0x909e89d5;

function namesBin(list: string[]): { buf: Buffer; off: Map<string, number> } {
  const parts: Buffer[] = [];
  const off = new Map<string, number>();
  let at = 0;
  for (const n of list) {
    const h = Buffer.alloc(4);
    h.writeUInt32LE(fableHash(n));
    off.set(n, at + 4);
    const s = Buffer.from(n + '\0', 'latin1');
    parts.push(h, s);
    at += 4 + s.length;
  }
  const body = Buffer.concat(parts);
  const head = Buffer.alloc(20);
  head.writeUInt32LE(body.length + 20, 0);
  head.writeUInt32LE(0x1234abcd, 4);
  head.writeUInt32LE(list.length, 8);
  return { buf: Buffer.concat([head, body]), off };
}

const u32 = (v: number) => { const b = Buffer.alloc(4); b.writeUInt32LE(v >>> 0); return b; };
const f32 = (v: number) => { const b = Buffer.alloc(4); b.writeFloatLE(v); return b; };
const field = (name: string | number, value: Buffer) => Buffer.concat([u32(typeof name === 'number' ? name : fableHash(name)), value]);

function make(dir: string): void {
  fs.mkdirSync(dir, { recursive: true });
  const types = ['CShopDef', 'CAppearanceModifierDef', 'CStockItemDef'];
  const defs: [string, string][] = [
    ['ARMOUR', 'ARMOUR_HERO_PLATE_ARMOUR'],
    ['ARMOUR', 'ARMOUR_HERO_LEATHER_ARMOUR'],
    ['ARMOUR', 'ARMOUR_HERO_PLATINUM_ARMOUR'],
    ['OBJECT', 'OBJECT_RESURRECTION_PHIAL'],
    ['OBJECT', 'OBJECT_HERO_SHIRT_PLATE'],
    ['OBJECT', 'OBJECT_HERO_SHIRT_LEATHER'],
    ['OBJECT', 'OBJECT_HEALTH_POTION'],
    ['BUILDING', 'BUILDING_TEST_SHOP'],
    ['OBJECT', 'OBJECT_HERO_HELMET_PLATINUM'],
  ];
  const fillers = 40;
  const allNames = [...types, ...types.map((t) => `NULLDEF_${t}`), 'ARMOUR', 'OBJECT', 'BUILDING', ...defs.map((d) => d[1])];
  const { buf: names, off } = namesBin(allNames);

  const T = types.length;
  const idx = (name: string) => T + defs.findIndex((d) => d[1] === name);
  const C0 = T + defs.length; // first component entry
  const comp = { plateLook: C0, leatherLook: C0 + 1, shop: C0 + 2, platinumLook: C0 + 3 };
  const rows: Buffer[] = [];
  const entries: Buffer[] = [];
  for (const t of types) { rows.push(Buffer.concat([u32(off.get(t)!), u32(off.get(`NULLDEF_${t}`)!), u32(0)])); entries.push(Buffer.from([1, 0, 0])); }
  const compRow = (type: string, entry: number, owner: number) => Buffer.concat([u32(fableHash(type)), u32(entry), u32(owner)]);
  defs.forEach(([group, name], i) => {
    rows.push(Buffer.concat([u32(off.get(group)!), u32(off.get(name)!), u32(i + 1)]));
    const me = T + i;
    const comps: Buffer[] = [];
    if (name === 'OBJECT_HERO_SHIRT_PLATE') comps.push(compRow('CAppearanceModifierDef', comp.plateLook, me));
    if (name === 'OBJECT_HERO_SHIRT_LEATHER') comps.push(compRow('CAppearanceModifierDef', comp.leatherLook, me));
    if (name === 'OBJECT_HERO_HELMET_PLATINUM') comps.push(compRow('CAppearanceModifierDef', comp.platinumLook, me));
    if (name === 'BUILDING_TEST_SHOP') comps.push(compRow('CShopDef', comp.shop, me));
    const n = Buffer.alloc(2);
    n.writeUInt16LE(comps.length);
    entries.push(Buffer.concat([Buffer.from([2, 0, 0]), n, ...comps, field('Health', f32(1))]));
  });
  const look = (armour: string) => Buffer.concat([Buffer.from([3, 0, 0]), field(0xdebc9e21, f32(0.4)), field(ARMOUR_FIELD, u32(idx(armour)))]);
  const stockRow = (item: string, q: number, m: number) =>
    Buffer.concat([field('Type', u32(idx(item))), field('Quantity', f32(q)), field('PriceMultiplier', f32(1.25)), field('MaxStock', f32(m)), field('RestockPeriod', u32(3))]);
  const shop = Buffer.concat([
    Buffer.from([3, 0, 0]),
    stockRow('OBJECT_RESURRECTION_PHIAL', 1, 1),
    stockRow('OBJECT_HERO_SHIRT_PLATE', 2, 2),
    stockRow('OBJECT_HERO_SHIRT_LEATHER', 3, 4),
    stockRow('OBJECT_HEALTH_POTION', 10, 20),
    stockRow('OBJECT_HERO_HELMET_PLATINUM', 1, 1),
  ]);
  const compType: [string, Buffer][] = [
    ['CAppearanceModifierDef', look('ARMOUR_HERO_PLATE_ARMOUR')],
    ['CAppearanceModifierDef', look('ARMOUR_HERO_LEATHER_ARMOUR')],
    ['CShopDef', shop],
    ['CAppearanceModifierDef', look('ARMOUR_HERO_PLATINUM_ARMOUR')],
  ];
  // Filler components: some compressible, some random, so chunks use all three block types.
  for (let i = 0; i < fillers; i++) {
    const body = i % 3 === 0 ? Buffer.from(Array.from({ length: 900 }, () => Math.floor(Math.random() * 256))) : Buffer.alloc(700 + i * 13, i);
    compType.push(['CStockItemDef', Buffer.concat([Buffer.from([3, 0, 0]), field(0x70395b57, u32(100 + i)), body])]);
  }
  compType.forEach(([t, e], i) => { rows.push(Buffer.concat([u32(off.get(t)!), u32(NONE), u32(i)])); entries.push(e); });

  // Chunks of 7 entries; the shop sits in the middle of one.
  const per = 7;
  const packed: Buffer[] = [];
  const table: [number, number][] = [];
  let dataOff = 0;
  for (let s = 0, k = 0; s < entries.length; s += per, k++) {
    const group = entries.slice(s, s + per);
    const head = Buffer.alloc(group.length * 2);
    let p = head.length;
    group.forEach((e, j) => { head.writeUInt16LE(p, j * 2); p += e.length; });
    const plain = Buffer.concat([head, ...group]);
    const z = zlib.deflateSync(plain, { level: [9, 1, 0, 6][k % 4]!, strategy: k % 5 === 4 ? zlib.constants.Z_FIXED : zlib.constants.Z_DEFAULT_STRATEGY });
    table.push([s, dataOff]);
    packed.push(z);
    dataOff += z.length;
  }
  table.push([entries.length, dataOff]);
  const head = Buffer.alloc(13);
  head[0] = 0;
  head.writeUInt32LE(77, 1);
  head.writeUInt32LE(names.readUInt32LE(4), 5);
  head.writeUInt32LE(entries.length, 9);
  const chunkTable = Buffer.concat([u32(table.length), ...table.flatMap(([a, b]) => [u32(a), u32(b)]), u32(dataOff)]);
  const game = Buffer.concat([head, ...rows, chunkTable, ...packed]);
  fs.writeFileSync(path.join(dir, 'game.bin'), game);
  fs.writeFileSync(path.join(dir, 'names.bin'), names);

  // Sanity: the server reader takes the fixture.
  const shops = extractShopTypes(new GameBin(game, new NamesBin(names)));
  assert.equal(shops.BUILDING_TEST_SHOP?.length, 5);

  // Inflate vectors: <n>.raw and <n>.z at every level and a fixed-Huffman stream.
  const raw = Buffer.concat([Buffer.alloc(5000, 7), Buffer.from(Array.from({ length: 70000 }, (_, i) => (i * 31 + (i >> 7)) & 0xff)), Buffer.from('the hero walks into the shop '.repeat(300))]);
  let n = 0;
  for (const level of [0, 1, 6, 9]) for (const strategy of [zlib.constants.Z_DEFAULT_STRATEGY, zlib.constants.Z_FIXED]) {
    fs.writeFileSync(path.join(dir, `vec${n}.raw`), raw);
    fs.writeFileSync(path.join(dir, `vec${n}.z`), zlib.deflateSync(raw, { level, strategy }));
    n++;
  }
  fs.writeFileSync(path.join(dir, 'vectors'), String(n));
  console.log(`fixture: ${entries.length} entries in ${table.length - 1} chunks, ${n} inflate vectors`);
}

function check(dir: string): void {
  const names = new NamesBin(fs.readFileSync(path.join(dir, 'names.bin')));
  const before = new GameBin(fs.readFileSync(path.join(dir, 'game.bin')), names);
  const after = new GameBin(fs.readFileSync(path.join(dir, 'filtered.bin')), names);
  const shops = extractShopTypes(after);
  assert.deepEqual(shops.BUILDING_TEST_SHOP?.map((r) => r.def), ['OBJECT_HERO_SHIRT_LEATHER', 'OBJECT_HEALTH_POTION']);
  const leather = shops.BUILDING_TEST_SHOP![0]!;
  assert.deepEqual(leather, { def: 'OBJECT_HERO_SHIRT_LEATHER', quantity: 3, maxStock: 4, priceMultiplier: 1.25, restockDays: 3 });
  // Every entry but the shop is byte for byte the same.
  const shopEntry = before.componentsOf(before.def('BUILDING_TEST_SHOP')!)[0]!.entry;
  for (let i = 0; i < before.types.length + before.defs.length + before.components.length; i++) {
    if (i !== shopEntry) assert.ok(before.entry(i).equals(after.entry(i)), `entry ${i} changed`);
  }
  console.log('check: banned rows empty, other rows and entries unchanged');
}

const [mode, dir] = process.argv.slice(2);
if (!dir) throw new Error('usage: shop_stock_check.ts make|check <dir>');
if (mode === 'make') make(dir);
else if (mode === 'check') check(dir);
else throw new Error(`unknown mode ${mode}`);
