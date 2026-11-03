import fs from 'node:fs';
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { MemorySink } from '../src/analytics/events.js';
import type { PlayerLink } from '../src/world/channel.js';
import { WorldThings, type WorldThingsFile } from '../src/world/things/world-things.js';
import { makeHero } from './helpers.js';
import { resolveLoot } from '../src/world/things/loot.js';
import { RegionTable } from '../src/world/regions.js';
import { decode, ProtocolError } from '../src/net/protocol.js';

const R = 'HeroGuildComplex';
const base = (id: string, pos: [number, number, number], region = R) => ({ id, uid: id.split(':')[1]!, def: 'X', level: region, region, section: 'NULL', script: null, pos });
const link = (region: string, pos: [number, number, number]) => ({ entrance: `${region}:1`, region, level: region, pos, fwd: [0, 1] as [number, number] });

function fixture(): WorldThingsFile {
  return {
    v: 1,
    exits: [
      { ...base(`${R}:10`, [0, 10, 0]), def: 'REGION_EXIT_POINT', active: true, radius: 3, to: link('LookoutPoint', [50, 50, 0]) },
      { ...base(`${R}:11`, [100, 10, 0]), def: 'REGION_EXIT_POINT', active: true, radius: 3, to: link('GuildWoods', [5, 5, 0]) },
    ],
    doors: [
      { ...base(`${R}:20`, [2, 0, 0]), def: 'OBJECT_BUILDING_DOOR_1', open: false, trigger: 0, owner: null, prison: false },
      { ...base(`${R}:21`, [0, 2, 0]), def: 'OBJECT_PRISON_CELL_DOOR_01', open: false, trigger: 0, owner: null, prison: true },
    ],
    chests: [
      { ...base(`${R}:30`, [1, 1, 0]), def: 'OBJECT_CHEST_OPENABLE', contents: ['OBJECT_GOLD_500', 'OBJECT_HEALTH_POTION'], open: false, silverKeys: 0, persistent: false },
      { ...base(`${R}:31`, [-1, 1, 0]), def: 'OBJECT_CHEST_OPENABLE', contents: ['OBJECT_HERO_HELMET_PLATINUM', 'OBJECT_GOLD_100'], open: false, silverKeys: 0, persistent: false },
      { ...base(`${R}:32`, [1, -1, 0]), def: 'OBJECT_SILVERKEY_CHEST_5', contents: ['OBJECT_LEGENDARY_KATANA'], open: false, silverKeys: 5, persistent: true },
      { ...base(`${R}:33`, [300, 300, 0]), def: 'OBJECT_CHEST_OPENABLE', contents: ['OBJECT_GOLD_50'], open: false, silverKeys: 0, persistent: false },
    ],
    containers: [],
    pickups: [
      { ...base(`${R}:40`, [-1, -1, 0]), def: 'OBJECT_APPLE_GREEN_01', forSale: false, stealable: true, price: 5, persistent: false },
      { ...base(`${R}:41`, [0, -1, 0]), def: 'OBJECT_SILVER_KEY', forSale: false, stealable: false, price: 0, persistent: true },
    ],
    digSpots: [],
    demonDoors: [{ ...base(`${R}:50`, [100, 12, 0]), def: 'CREATURE_DEMON_DOOR_FACE_01', exit: `${R}:11` }],
    hooks: [{ ...base(`${R}:60`, [-2, 0, 0]), def: 'OBJECT_PASSAGE_DOOR_01', to: link('ArenaExterior', [7, 7, 0]) }],
  };
}

function setup() {
  const sink = new MemorySink();
  const app = createApp({ dbPath: ':memory:', eventsPath: '', regionsPath: '/nonexistent', thingsPath: '' }, { events: sink, things: new WorldThings(fixture()) });
  return Object.assign(app, { sink });
}

function join(app: ReturnType<typeof setup>, at: [number, number, number] = [0, 0, 0]) {
  const { hero } = makeHero(app, 'world');
  app.heroes.savePosition(hero.id, R, ...at);
  const inbox: any[] = [];
  const l: PlayerLink = { heroId: hero.id, name: hero.name, renown: 0, lastSeq: -1, joinedAt: Date.now(), send: (f) => inbox.push(JSON.parse(f)) };
  app.director.join(app.heroes.get(hero.id)!, l);
  return { hero, inbox, id: hero.id };
}

let seq = 0;
const walk = (app: ReturnType<typeof setup>, heroId: number, moveY: number) => app.director.input(heroId, { seq: ++seq, moveX: 0, moveY, buttons: 0, cameraYaw: 0 });

test('a chest pays each hero once, through the ledger', () => {
  const app = setup();
  const a = join(app);
  const b = join(app);
  const first = app.objects.interact(a.id, `${R}:30`);
  assert.equal(first.t, 'loot');
  assert.equal(first.gold, 500);
  assert.deepEqual((first.items as any[]).map((i) => i.def), ['HealthPotion']);
  assert.equal(app.economy.balance(a.id), 500);
  // Again (a double click, a resend, a reconnect, another channel): nothing new.
  const again = app.objects.interact(a.id, `${R}:30`);
  assert.equal(again.again, true);
  assert.equal(again.gold, 0);
  assert.equal(app.economy.balance(a.id), 500);
  assert.equal(app.economy.inventory(a.id).length, 1);
  // Another hero has their own copy of the chest.
  assert.equal(app.objects.interact(b.id, `${R}:30`).gold, 500);
  assert.ok(app.economy.audit().ok);
  app.close();
});

test('loot is checked against the server position, not the client', () => {
  const app = setup();
  const a = join(app);
  assert.equal(app.objects.interact(a.id, `${R}:33`).error, 'too_far');
  assert.equal(app.objects.interact(a.id, 'Nowhere:1').error, 'not_here');
  assert.equal(app.objects.interact(a.id, `${R}:10`).error, 'walk_through');
  assert.equal(app.economy.balance(a.id), 0);
  app.close();
});

test('staff-only platinum never comes out of a chest', () => {
  const app = setup();
  const a = join(app);
  const r = app.objects.interact(a.id, `${R}:31`);
  assert.deepEqual(r.blocked, ['OBJECT_HERO_HELMET_PLATINUM']);
  assert.equal(r.gold, 100);
  assert.equal(app.economy.inventory(a.id).length, 0);
  assert.deepEqual(resolveLoot(['OBJECT_HERO_BOOTS_PLATINUM'], 'x'), [{ blocked: 'OBJECT_HERO_BOOTS_PLATINUM' }]);
  app.close();
});

test('silver key chests need the keys, which are per hero and not tradeable', () => {
  const app = setup();
  const a = join(app);
  assert.equal(app.objects.interact(a.id, `${R}:32`).error, 'need_keys');
  // The silver key on the ground is unique: each hero takes it once, in their own world.
  assert.equal(app.objects.interact(a.id, `${R}:41`).t, 'loot');
  assert.equal(app.objects.interact(a.id, `${R}:41`).error, 'gone');
  const b = join(app);
  assert.equal(app.objects.interact(b.id, `${R}:41`).t, 'loot');
  assert.equal(app.economy.catalog.get('OBJECT_SILVER_KEY').tradeable, false);
  for (let i = 0; i < 4; i++) app.economy.claimLoot(a.id, `test-keys-${i}-${a.id}`, 0, [{ defId: 'OBJECT_SILVER_KEY', quantity: 1 }], 'test');
  const r = app.objects.interact(a.id, `${R}:32`);
  assert.deepEqual((r.items as any[]).map((i) => i.def), ['OBJECT_LEGENDARY_KATANA']);
  assert.equal(app.economy.countOf(a.id, 'OBJECT_SILVER_KEY'), 5, 'keys are not used up');
  app.close();
});

test('a common pickup goes to whoever reaches it first and is gone for the channel', () => {
  const app = setup();
  const a = join(app);
  const b = join(app);
  assert.equal(app.objects.interact(a.id, `${R}:40`).t, 'loot');
  assert.ok(b.inbox.some((m) => m.t === 'object' && m.id === `${R}:40` && m.gone === true));
  assert.equal(app.objects.interact(b.id, `${R}:40`).error, 'gone');
  assert.ok(app.sink.events.some((e) => e.type === 'theft' && e.heroId === a.id));
  // A hero arriving later sees it gone too.
  const c = join(app);
  assert.ok(c.inbox.find((m) => m.t === 'objects').gone.includes(`${R}:40`));
  app.close();
});

test('doors are shared by the channel and swing shut again; prison doors stay locked', () => {
  const app = setup();
  const a = join(app);
  const b = join(app);
  assert.equal(app.objects.interact(a.id, `${R}:20`).open, true);
  assert.ok(b.inbox.some((m) => m.t === 'object' && m.id === `${R}:20` && m.open === true));
  const c = join(app);
  assert.deepEqual(c.inbox.find((m) => m.t === 'objects').doors, [[`${R}:20`, true]]);
  assert.equal(app.objects.interact(a.id, `${R}:20`, 'close').open, false);
  assert.equal(app.objects.interact(a.id, `${R}:21`).error, 'locked');
  // Opened and left: it closes by itself.
  app.objects.interact(a.id, `${R}:20`, 'open');
  const realNow = Date.now;
  Date.now = () => realNow() + 60_000;
  try {
    app.director.tick(50);
  } finally {
    Date.now = realNow;
  }
  assert.equal(b.inbox.filter((m) => m.t === 'object' && m.id === `${R}:20`).at(-1).open, false);
  app.close();
});

test('walking into a region exit hands the hero to the next region and holds them until it has loaded', () => {
  const app = setup();
  const a = join(app);
  app.director.tick(50); // standing clear of every exit: armed
  walk(app, a.id, 1);
  for (let i = 0; i < 3; i++) app.director.tick(500); // 0 -> 9 m, into the exit at y = 10
  const enter = a.inbox.filter((m) => m.t === 'enter_region').at(-1);
  assert.equal(enter.region, 'LookoutPoint');
  assert.equal(enter.from, R);
  assert.equal(enter.via, 'exit');
  assert.equal(enter.entrance, 'LookoutPoint:1');
  assert.equal(enter.loading, true);
  const ch = app.director.channelOf(a.id)!;
  assert.equal(ch.region.name, 'LookoutPoint');
  assert.deepEqual(app.heroes.get(a.id)!.region, 'LookoutPoint');
  // Inputs are ignored while the client loads the region...
  walk(app, a.id, 1);
  app.director.tick(1000);
  assert.deepEqual(ch.sim.position(a.id), { x: 50, y: 50, z: 0 });
  // ...and work once it says it's ready.
  app.director.regionReady(a.id, 'LookoutPoint');
  walk(app, a.id, 1);
  app.director.tick(1000);
  assert.equal(ch.sim.position(a.id)!.y, 56);
  assert.ok(app.sink.events.some((e) => e.type === 'region_exit' && e.heroId === a.id && e.to === 'LookoutPoint'));
  app.close();
});

test('arriving on top of an exit does not bounce the hero straight back', () => {
  const app = setup();
  const a = join(app, [0, 9, 0]); // inside the exit's radius
  app.director.tick(50);
  app.director.tick(50);
  assert.equal(app.director.channelOf(a.id)!.region.name, R);
  app.close();
});

test('a demon door keeps its exit shut until it has opened for that hero', () => {
  const app = setup();
  const a = join(app, [100, 0, 0]);
  app.director.tick(50);
  walk(app, a.id, 1);
  for (let i = 0; i < 3; i++) app.director.tick(500);
  assert.equal(app.director.channelOf(a.id)!.region.name, R);
  assert.ok(a.inbox.some((m) => m.error === 'demon_door_closed'));
  // The riddle runs in the hero's own game; the client reports the door opened.
  assert.equal(app.objects.interact(a.id, `${R}:50`, 'use').error, 'bad_action');
  assert.equal(app.objects.interact(a.id, `${R}:50`, 'opened').opened, true);
  // Step back out and in again.
  walk(app, a.id, -1);
  for (let i = 0; i < 2; i++) app.director.tick(500);
  walk(app, a.id, 1);
  for (let i = 0; i < 2; i++) app.director.tick(500);
  assert.equal(app.director.channelOf(a.id)!.region.name, 'GuildWoods');
  // Another hero still finds it shut.
  const b = join(app, [100, 0, 0]);
  assert.ok(!app.objects.hasFlag(b.id, `demon_door:${R}:50`));
  app.close();
});

test('a door that leads to another region moves the hero there', () => {
  const app = setup();
  const a = join(app);
  assert.equal(app.objects.interact(a.id, `${R}:60`).t, 'ok');
  assert.equal(app.director.channelOf(a.id)!.region.name, 'ArenaExterior');
  assert.equal(a.inbox.filter((m) => m.t === 'enter_region').at(-1).via, 'door');
  app.close();
});

test('a hero sees the chests they already emptied when they come back', () => {
  const app = setup();
  const a = join(app);
  app.objects.interact(a.id, `${R}:30`);
  app.director.leave(a.id);
  const l: PlayerLink = { heroId: a.id, name: a.hero.name, renown: 0, lastSeq: -1, joinedAt: Date.now(), send: (f) => a.inbox.push(JSON.parse(f)) };
  app.heroes.savePosition(a.id, R, 0, 0, 0);
  app.director.join(app.heroes.get(a.id)!, l);
  assert.deepEqual(a.inbox.filter((m) => m.t === 'objects').at(-1).opened, [`${R}:30`]);
  app.close();
});

const THINGS = '../shared/defs/world-things.json';
const REGIONS = '../shared/defs/regions.json';

test('extracted data: every exit leads to a neighbouring region (needs your game files)', { skip: !fs.existsSync(THINGS) || !fs.existsSync(REGIONS) }, () => {
  const raw = JSON.parse(fs.readFileSync(THINGS, 'utf8')) as WorldThingsFile;
  const regions = RegionTable.load(REGIONS);
  const exits = raw.exits.filter((e) => e.active && e.to && e.section === 'NULL');
  assert.ok(exits.length > 100);
  for (const e of exits) assert.ok(regions.areNeighbours(e.region, e.to!.region), `${e.id}: ${e.region} -> ${e.to!.region}`);
  for (const d of raw.demonDoors) assert.ok(d.exit, `${d.id} guards an exit`);
});

test('interact and region_ready frames are validated', () => {
  assert.deepEqual(decode('{"t":"interact","thing":"LookoutPoint:112"}'), { t: 'interact', thing: 'LookoutPoint:112', action: 'use' });
  assert.equal((decode('{"t":"interact","thing":"A:1","action":"give_me_gold"}') as any).action, 'use');
  assert.throws(() => decode('{"t":"interact","thing":"../../etc"}'), ProtocolError);
  assert.throws(() => decode('{"t":"interact","thing":42}'), ProtocolError);
  assert.deepEqual(decode('{"t":"region_ready","region":"PicnicArea"}'), { t: 'region_ready', region: 'PicnicArea' });
});
