import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WorldClock, climateOf, zoneOf } from '../src/world/clock.js';
import { FALLBACK_TOWN } from '../src/world/town/town-data.js';
import { NPC_ID_BASE, NpcAnim, TownLife, shopIdOf } from '../src/world/town/townsfolk.js';
import { TownSim, townOf, withTown } from '../src/world/town/town-sim.js';
import { StubSim } from '../src/world/sim.js';
import { extractShopTypes, extractVillagePeople, parseTownTng } from '../src/data/town-extract.js';
import { GameBin, NamesBin } from '../src/data/fable-bin.js';
import { buildBins } from './fixtures/fable-bin-builder.js';
import { seeded } from './town-helpers.js';

const DAY = 60 * 60_000;
/** A clock whose time the test controls. Noon at t = 0. */
function testClock(startFraction = 0.5) {
  let t = 0;
  const clock = new WorldClock({ dayMs: DAY, epoch: 0, startFraction, weatherSlotMs: 60_000, seed: 7 }, () => t);
  return { clock, advance: (ms: number) => (t += ms), set: (ms: number) => (t = ms), now: () => t };
}

// ---------------------------------------------------------------- clock

test('clock: time of day is a pure function of wall time', () => {
  const a = new WorldClock({ dayMs: DAY, epoch: 0, startFraction: 0 });
  const b = new WorldClock({ dayMs: DAY, epoch: 0, startFraction: 0 });
  assert.equal(a.dayFraction(DAY / 2), 0.5);
  assert.equal(a.hour(DAY / 4), 6);
  assert.equal(a.dayFraction(DAY * 3 + DAY / 4), 0.25);
  assert.equal(a.day(DAY * 3 + 1), 3);
  assert.equal(a.isNight(DAY * 0.1), true);
  assert.equal(a.isNight(DAY * 0.5), false);
  assert.equal(a.isNight(DAY * 0.9), true);
  for (const t of [0, 12_345, 9_999_999]) assert.deepEqual(a.sky('Greatwood_3', t), b.sky('Greatwood_3', t));
});

test('clock: weather follows climate, zones share it, and it changes over time', () => {
  const c = new WorldClock({ dayMs: DAY, epoch: 0, weatherSlotMs: 60_000, seed: 3 });
  assert.equal(climateOf('HeroGuildComplexInside'), 'indoor');
  assert.equal(climateOf('DarkwoodChapel'), 'indoor');
  assert.equal(climateOf('SnowspireVillage'), 'snow');
  assert.equal(climateOf('Darkwood_4'), 'gloom');
  assert.equal(climateOf('HookCoast'), 'coast');
  assert.equal(climateOf('OakValeWest_v2'), 'temperate');
  assert.equal(zoneOf('Darkwood_4'), zoneOf('Darkwood_10'));
  assert.notEqual(zoneOf('Darkwood_4'), zoneOf('Greatwood_3'));
  const seen = new Set<string>();
  for (let slot = 0; slot < 200; slot++) {
    const t = slot * 60_000 + 1;
    assert.equal(c.weatherAt('Darkwood_4', t).weather, c.weatherAt('Darkwood_10', t).weather);
    assert.equal(c.weatherAt('BowerstoneJail', t).weather, 'clear');
    const w = c.weatherAt('SnowspireVillage', t).weather;
    assert.ok(['cloudy', 'snow', 'blizzard', 'clear'].includes(w), w);
    seen.add(c.weatherAt('Greatwood_3', t).weather);
  }
  assert.ok(seen.size >= 4, `temperate weather varies: ${[...seen]}`);
  const s = c.sky('Greatwood_3', 90_000);
  assert.deepEqual([s.since, s.until, s.day_ms], [60_000, 120_000, DAY]);
  assert.equal(s.previous, c.weatherAt('Greatwood_3', 59_999).weather);
});

test('clock: sky key changes at a weather roll and at dusk, not in between', () => {
  const { clock } = testClock(0);
  const k = (t: number) => clock.skyKey('Greatwood_3', t);
  assert.equal(k(1000), k(59_000));
  assert.notEqual(k(59_000), k(61_000));
  const dusk = (20 / 24) * DAY;
  assert.notEqual(k(dusk - 1), k(dusk + 1));
});

// ---------------------------------------------------------------- town folk

test('town: a village gets shopkeepers at its shops, residents and placed traders', () => {
  const { clock } = testClock();
  const town = new TownLife('TestRegion', FALLBACK_TOWN, clock, undefined, { rng: seeded(1) });
  const roles = [...town.npcs.values()].map((n) => `${n.role}:${n.def}`).sort();
  assert.deepEqual(roles, [
    'shopkeeper:CREATURE_TEST_SHOPKEEPER',
    'shopkeeper:CREATURE_TEST_SHOPKEEPER',
    'trader:CREATURE_TEST_TRADER',
    'villager:CREATURE_TEST_VILLAGER_FEMALE',
    'villager:CREATURE_TEST_VILLAGER_MALE',
    'villager:CREATURE_TEST_VILLAGER_MALE',
  ]);
  assert.ok([...town.npcs.keys()].every((id) => id >= NPC_ID_BASE));
  assert.ok(town.shopOpen(shopIdOf('TestRegion', 's1')));
  assert.equal(town.keeperOf(shopIdOf('TestRegion', 's2'))?.role, 'shopkeeper');
  assert.equal(new TownLife('Nowhere', FALLBACK_TOWN, clock).npcs.size, 0);
});

test('town: villagers walk about by day, go indoors at night and come out at dawn', () => {
  const tc = testClock(0.5);
  const town = new TownLife('TestRegion', FALLBACK_TOWN, tc.clock, undefined, { rng: seeded(2), nightOwlShare: 0 });
  const villagers = [...town.npcs.values()].filter((n) => n.role === 'villager');
  const start = villagers.map((v) => ({ ...v.pos }));
  const run = (ms: number) => {
    for (let t = 0; t < ms; t += 100) {
      tc.advance(100);
      town.step(100, [], []);
    }
  };
  run(30_000);
  assert.ok(villagers.some((v, i) => Math.hypot(v.pos.x - start[i]!.x, v.pos.y - start[i]!.y) > 1), 'someone went for a walk');
  for (const v of villagers) assert.ok(Math.hypot(v.pos.x - v.post.x, v.pos.y - v.post.y) <= 18 + 0.5, 'stays near the village');

  tc.set((21 / 24 - 0.5) * DAY); // 21:00
  run(60_000);
  assert.ok(villagers.every((v) => v.state === 'indoors'), villagers.map((v) => v.state).join());
  const shown = new Set(town.snapshot().map((e) => e.id));
  assert.ok(villagers.every((v) => !shown.has(v.id)), 'indoor villagers are not sent to clients');
  assert.ok(town.shopOpen(shopIdOf('TestRegion', 's1')), 'shops stay open at night');

  tc.set((1 + 7 / 24 - 0.5) * DAY); // 07:00 next day
  run(1000);
  assert.ok(villagers.every((v) => v.state !== 'indoors'));
  assert.ok(villagers.every((v) => shownIn(town, v.id)));
});

test('town: a fight sends villagers running and shopkeepers ducking; the shop shuts until it is over', () => {
  const tc = testClock(0.5);
  const town = new TownLife('TestRegion', FALLBACK_TOWN, tc.clock, undefined, { rng: seeded(3) });
  const shop = shopIdOf('TestRegion', 's1');
  const keeper = town.keeperOf(shop)!;
  const near = [...town.npcs.values()].find((n) => n.role === 'villager')!;
  const fight = { x: keeper.pos.x + 2, y: keeper.pos.y, z: 0 };
  near.pos = { x: fight.x + 1, y: fight.y, z: 0 };
  town.step(100, [], [fight]);
  assert.equal(keeper.state, 'cower');
  assert.equal(keeper.anim, NpcAnim.Cower);
  assert.equal(town.shopOpen(shop), false);
  assert.equal(near.state, 'flee');
  for (let i = 0; i < 20; i++) town.step(100, [], []);
  assert.ok(Math.hypot(near.pos.x - fight.x, near.pos.y - fight.y) > 5, 'ran away from the fight');
  for (let i = 0; i < 100; i++) town.step(100, [], []);
  assert.equal(town.shopOpen(shop), true, 'keeper back at the counter');
  assert.equal(town.witnesses(keeper.pos, 3).includes(keeper), true);
});

test('town: an idle villager turns to face a hero who walks up', () => {
  const tc = testClock(0.5);
  const town = new TownLife('TestRegion', FALLBACK_TOWN, tc.clock, undefined, { rng: seeded(4), idleMs: [60_000, 60_000] });
  const v = [...town.npcs.values()].find((n) => n.role === 'villager')!;
  const hero = { x: v.pos.x + 2, y: v.pos.y, z: 0 };
  for (let i = 0; i < 5; i++) town.step(100, [hero], []);
  assert.equal(v.anim, NpcAnim.Greet);
  assert.ok(Math.abs(v.yaw - Math.atan2(-2, 0)) < 1e-6, `yaw ${v.yaw}`);
});

test('town sim: wraps any sim, adds town folk after heroes, and monsters mid-fight scare villagers', () => {
  const tc = testClock(0.5);
  const region = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };
  const factory = withTown(() => new StubSim(), { data: FALLBACK_TOWN, clock: tc.clock, options: { rng: seeded(5) } });
  const sim = factory(region, 'TestRegion#1');
  assert.ok(sim instanceof TownSim);
  assert.ok(!(withTown(() => new StubSim(), { data: FALLBACK_TOWN, clock: tc.clock })({ ...region, name: 'Elsewhere' }, 'x') instanceof TownSim));
  sim.addHero(1, { x: 40, y: 52, z: 0 });
  sim.step(50);
  const snap = sim.snapshot();
  assert.equal(snap[0]!.id, 1);
  assert.equal(snap.length, 1 + 6);
  // The hero swings a sword right by the general store.
  sim.applyInput(1, { seq: 1, moveX: 0, moveY: 0, buttons: 1, cameraYaw: 0 });
  sim.step(50);
  assert.equal(townOf(sim)!.shopOpen(shopIdOf('TestRegion', 's1')), false);
  assert.equal(sim.position(1)?.x, 40);
  sim.dispose();
});

// ---------------------------------------------------------------- extraction

test('extract: shop stock rows and village roles from game.bin; towns from .tng text', () => {
  const { game, names } = buildBins(
    ['CShopDef', 'CVillagePeopleDef'],
    [
      { group: 'OBJECT', name: 'OBJECT_SWORD_A' },
      { group: 'OBJECT', name: 'OBJECT_POTION_B' },
      { group: 'CREATURE', name: 'CREATURE_KEEPER_A' },
      { group: 'CREATURE', name: 'CREATURE_MAN_A' },
      { group: 'CREATURE', name: 'CREATURE_GUARD_A' },
      {
        group: 'BUILDING',
        name: 'BUILDING_SHOP_A',
        components: [
          {
            type: 'CShopDef',
            fields: [
              { name: 'Name', i32: 0 },
              { name: 'Type', ref: 'OBJECT_SWORD_A' },
              { name: 'Quantity', f32: 0 },
              { name: 'PriceMultiplier', f32: 1.2 },
              { name: 'MaxStock', f32: 2 },
              { name: 'RestockPeriod', i32: 3 },
              { name: 'Type', ref: 'OBJECT_POTION_B' },
              { name: 'Quantity', f32: 5 },
              { name: 'PriceMultiplier', f32: 1 },
              { name: 'MaxStock', f32: 9 },
              { name: 'RestockPeriod', i32: 1 },
              { name: 'Type', ref: 'OBJECT_SWORD_A' },
              { name: 'Quantity', f32: 0 },
              { name: 'MaxStock', f32: 0 },
            ],
          },
        ],
      },
      {
        group: 'VILLAGE',
        name: 'VILLAGE_A',
        components: [{ type: 'CVillagePeopleDef', fields: [{ name: 'MaleShopKeeper', ref: 'CREATURE_KEEPER_A' }, { name: 'MaleUnemployed', ref: 'CREATURE_MAN_A' }, { name: 'Guard', ref: 'CREATURE_GUARD_A' }] }],
      },
    ],
  );
  const g = new GameBin(game, new NamesBin(names));
  assert.deepEqual(extractShopTypes(g), {
    BUILDING_SHOP_A: [
      { def: 'OBJECT_SWORD_A', quantity: 0, maxStock: 2, priceMultiplier: 1.2, restockDays: 3 },
      { def: 'OBJECT_POTION_B', quantity: 5, maxStock: 9, priceMultiplier: 1, restockDays: 1 },
    ],
  });
  assert.deepEqual(extractVillagePeople(g), { VILLAGE_A: { roles: { MaleShopKeeper: 'CREATURE_KEEPER_A', MaleUnemployed: 'CREATURE_MAN_A' }, guard: 'CREATURE_GUARD_A' } });

  const thing = (kind: string, def: string, uid: string, extra = '') =>
    `NewThing ${kind};\nUID ${uid};\nDefinitionType "${def}";\nScriptName NULL;\nPositionX 10.5;\nPositionY 20.0;\nPositionZ 1.0;\nRHSetForwardX 0.0;\nRHSetForwardY 1.0;\n${extra}EndThing;\n`;
  const tng = [
    thing('Village', 'VILLAGE_A', '100'),
    thing('Building', 'BUILDING_SHOP_A', '101', 'StartCTCVillageMember;\nVillageUID 100;\nEndCTCVillageMember;\nStartCTCShop;\nEndCTCShop;\nIsResidential TRUE;\n'),
    thing('Building', 'BUILDING_HOUSE_A', '102', 'StartCTCVillageMember;\nVillageUID 100;\nEndCTCVillageMember;\nIsResidential TRUE;\n'),
    thing('Building', 'BUILDING_BARN_A', '103', 'IsResidential FALSE;\n'),
    thing('AICreature', 'CREATURE_MAN_A', '104'),
    thing('AICreature', 'CREATURE_BANDIT', '105'),
    thing('AICreature', 'CREATURE_GUARD_A', '106'),
    thing('AICreature', 'CREATURE_MAN_A', '107').replace('ScriptName NULL', 'ScriptName QuestMan'),
  ].join('');
  const r = parseTownTng(tng, (d) => (d === 'CREATURE_BANDIT' ? 'FACTION_BANDITS' : 'FACTION_VILLAGERS'));
  assert.deepEqual(r.villages, [{ uid: '100', def: 'VILLAGE_A', x: 10.5, y: 20, z: 1 }]);
  assert.deepEqual(r.shops.map((s) => [s.uid, s.village]), [['101', '100']]);
  assert.deepEqual(r.homes.map((h) => h.uid), ['102']);
  assert.deepEqual(r.villagers.map((v) => v.uid), ['104']);
});

function shownIn(town: TownLife, id: number): boolean {
  return town.snapshot().some((e) => e.id === id);
}
