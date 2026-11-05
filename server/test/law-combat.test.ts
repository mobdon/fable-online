import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { CombatSim, GUARD_RESPAWN_MS, TOWN_WATCH } from '../src/world/combat-sim.js';
import { monsterAi } from '../src/world/ai/combat-ai.js';
import { withTown } from '../src/world/town/town-sim.js';
import { FALLBACK_TOWN } from '../src/world/town/town-data.js';
import { WorldClock } from '../src/world/clock.js';
import { Law, type LawHost } from '../src/law/law.js';
import { mergeRules } from '../src/law/rules.js';
import type { RegionSim } from '../src/world/sim.js';
import { key, makeHero, testApp } from './helpers.js';
import { seeded } from './town-helpers.js';

const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };
const TICK = 50;

/** The fallback test village under combat, town life and a town watch, as the server runs it. */
function world(over: object = {}, opts: { monsters?: boolean } = {}) {
  const app = testApp();
  app.economy.policies.length = 0;
  let now = Date.UTC(2026, 10, 3, 12);
  const moves: { heroId: number; region: string }[] = [];
  const host: LawHost = {
    regionOf: () => 'TestRegion',
    relocate: (heroId, region) => void moves.push({ heroId, region }),
    notify: () => {},
    hasRegion: () => true,
  };
  const rules = mergeRules({ jurisdictions: [{ id: 'testvale', regions: ['TestRegion'], kickTo: 'Elsewhere' }], ...over });
  const law = new Law(app.db, app.economy, app.sink, rules, host, () => now, () => 0.5);
  const clock = new WorldClock({ dayMs: 3600_000, epoch: 0, startFraction: 0.5, weatherSlotMs: 60_000, seed: 7 }, () => 0);
  const factory = withTown(
    (region, channelId) => new CombatSim(region, channelId, {
        data: D,
        spawns: { regions: {} },
        events: app.sink,
        economy: app.economy,
        law: { law, events: app.sink, rng: () => 0 },
        ...(opts.monsters ? { ai: monsterAi() } : {}),
      }),
    { data: FALLBACK_TOWN, clock, options: { rng: seeded(3) } },
  );
  const sim = factory(REGION, 'TestRegion#1') as RegionSim & { inner: CombatSim };
  const combat = sim.inner;
  const ids = new Set<number>();
  const run = (ms: number) => {
    for (let t = 0; t < ms; t += TICK) {
      now += TICK;
      sim.step(TICK);
      law.tick(TICK, [...ids]);
    }
  };
  const hero = (at: { x: number; y: number }, gold = 0) => {
    const { hero } = makeHero(app);
    if (gold) app.economy.grant(hero.id, gold, 'quest_reward', key());
    sim.addHero(hero.id, { ...at, z: 0 });
    ids.add(hero.id);
    return hero.id;
  };
  const guards = () => [...combat.combat.all.values()].filter((c) => c.kind === 'creature' && c.faction === 'guard');
  return { app, law, sim, combat, run, hero, guards, moves };
}

test('the town layer posts guards on the village guard beats, and they show in the snapshot', () => {
  const w = world();
  const g = w.guards();
  assert.equal(g.length, 2);
  // FALLBACK_TOWN's two beats start at (40, 45) and (50, 40).
  const starts = g.map((c) => `${c.pos.x},${c.pos.y}`).sort();
  assert.deepEqual(starts, ['40,45', '50,40']);
  w.run(TICK);
  const snap = w.sim.snapshot();
  for (const c of g) assert.ok(snap.some((e) => e.id === c.id));
});

test('a hero who hits a guard has assaulted the watch, is chased and arrested', () => {
  const w = world();
  const [g] = w.guards();
  const h = w.hero({ x: g!.pos.x, y: g!.pos.y - 1 });
  const r = w.combat.combat.attack(h, g!.id, 'light');
  assert.ok(r.ok && r.hit, JSON.stringify(r));
  assert.equal(w.law.openCrimes(h)[0]?.kind, 'assault');
  assert.ok(w.law.isResisting(h));
  w.run(4000);
  assert.ok(w.law.openSentence(h), 'arrested');
  assert.equal(w.moves.at(-1)?.region, 'BowerstoneJail');
});

test('a wanted hero felled by a guard is arrested, keeps their gold and is not left dead', () => {
  // No arrest by holding, so the only way in is the guard's blow.
  const w = world({ guards: { ...mergeRules({}).guards, arrestHoldMs: 1e9 } });
  const [g] = w.guards();
  const h = w.hero({ x: g!.pos.x + 30, y: g!.pos.y }, 1000);
  w.law.record(h, 'murder', { town: 'testvale' });
  const hero = w.combat.hero(h)!;
  hero.health = 1;
  // Give the guard a clear shot: right next to the hero, facing it.
  g!.pos = { x: hero.pos.x, y: hero.pos.y - 1, z: 0 };
  g!.yaw = 0;
  w.law.markResisting(h);
  for (let i = 0; i < 40 && !w.law.openSentence(h); i++) w.run(250);
  assert.ok(w.law.openSentence(h));
  assert.ok(w.app.sink.events.some((e) => e.type === 'arrest' && e.heroId === h && e.reason === 'subdued'));
  assert.ok(hero.alive);
  assert.equal(w.app.economy.balance(h), 1000, 'no death penalty');
  assert.ok(!w.app.sink.events.some((e) => e.type === 'hero_death' && e.heroId === h));
});

test('killing a guard pays nothing and the post is filled again', () => {
  const w = world();
  const [g] = w.guards();
  const h = w.hero({ x: g!.pos.x, y: g!.pos.y - 1 });
  g!.health = 1;
  w.combat.combat.attack(h, g!.id, 'light');
  assert.equal(g!.alive, false);
  assert.equal(w.law.openCrimes(h).at(-1)?.kind, 'murder');
  assert.ok(!w.app.sink.events.some((e) => e.type === 'kill'));
  w.sim.removeHero(h); // keep the other guard from arresting during the wait
  w.run(GUARD_RESPAWN_MS + 500);
  assert.equal(w.guards().filter((c) => c.alive).length, 2);
});

test('town folk are witnesses: a lockpick right next to a villager reaches the guards', () => {
  const w = world();
  w.run(TICK);
  const v = [...(w.sim as unknown as { town: { npcs: Map<number, { pos: { x: number; y: number }; state: string; def: string }> } }).town.npcs.values()].find(
    (n) => n.state !== 'indoors' && /VILLAGER/.test(n.def),
  )!;
  // Far from both guards, so only the villager can see it.
  const h = w.hero({ x: v.pos.x + 1, y: v.pos.y });
  for (const g of w.guards()) g.pos = { x: 0, y: 0, z: 0 };
  w.sim.law!.report(h, 'lockpicking');
  assert.equal(w.law.openCrimes(h).length, 0);
  w.run(5000);
  assert.equal(w.law.openCrimes(h)[0]?.kind, 'lockpicking');
});

// ---------------------------------------------------------------- the watch on its beat and against monsters

test('guards walk their beat from the level files, end to end and back', () => {
  const w = world();
  const g = w.guards().find((c) => c.pos.x === 40 && c.pos.y === 45)!; // beat (40,45) <-> (60,45)
  let maxX = g.pos.x;
  for (let i = 0; i < 60; i++) {
    w.run(500);
    maxX = Math.max(maxX, g.pos.x);
  }
  assert.ok(maxX > 59, `reached the far end of the beat (max x ${maxX.toFixed(1)})`);
  for (let i = 0; i < 60 && g.pos.x > 41; i++) w.run(500);
  assert.ok(g.pos.x < 41.5, `came back (x ${g.pos.x.toFixed(1)})`);
  assert.ok(Math.abs(g.pos.y - 45) < 0.5, 'stayed on the beat');
});

test('a monster that comes into town is fought by the watch, which fights back', () => {
  const w = world({}, { monsters: true });
  // A troll that wandered into the middle of town, with no hero about: only the watch is there to fight.
  const troll = w.combat.spawns.spawnExtra('CREATURE_EARTH_TROLL', { x: 50, y: 47, z: 0 }, 0)!;
  const hp0 = troll.health;
  const guardHp0 = w.guards().reduce((n, g) => n + g.health, 0);
  w.run(10_000);
  assert.ok(w.guards().some((g) => g.targetId === troll.id) || !troll.alive, 'guards went for it');
  assert.ok(troll.health < hp0, 'guards hurt it');
  assert.ok(w.guards().reduce((n, g) => n + g.health, 0) < guardHp0 || !troll.alive, 'it fought back');
});

test('a monster outside town is left alone, and a guard drops one that runs out of town', () => {
  const w = world({}, { monsters: true });
  const h = w.hero({ x: 50, y: 150 }); // far north of the village outline (25..75)
  w.combat.hero(h)!.yaw = 0;
  w.combat.devSpawn(h, 'CREATURE_WOLF', 10);
  const wolf = w.combat.spawns.creatures().next().value!;
  assert.ok(!w.combat.inTown(wolf), 'out of town');
  w.run(3000);
  assert.ok(w.guards().every((g) => g.targetId !== wolf.id), 'no guard was sent');
  assert.equal(wolf.health, wolf.maxHealth);
});

test('a monster the watch killed pays the hero who only scratched it nothing', () => {
  const w = world({}, { monsters: true });
  const h = w.hero({ x: 50, y: 30 });
  w.combat.hero(h)!.yaw = 0;
  w.combat.devSpawn(h, 'CREATURE_WOLF', 15);
  const wolf = w.combat.spawns.creatures().next().value!;
  // Not a dev spawn any more: treat it as a real monster that wandered in.
  (w.combat as unknown as { devSpawned: Set<number> }).devSpawned.delete(wolf.id);
  const [g] = w.guards();
  // One scratch from the hero, standing right by it...
  w.combat.placeHero(h, { x: wolf.pos.x, y: wolf.pos.y - 1, z: 0 }, 0);
  const hit = w.combat.combat.attack(h, wolf.id, 'light');
  assert.ok(hit.ok && hit.hit, JSON.stringify(hit));
  w.combat.placeHero(h, { x: 50, y: 30, z: 0 });
  const goldBefore = w.app.economy.balance(h);
  // ...and the watch does the rest.
  for (let i = 0; i < 40 && wolf.alive; i++) {
    g!.pos = { x: wolf.pos.x, y: wolf.pos.y - 1, z: 0 };
    g!.yaw = 0;
    g!.attackCooldownMs = 0;
    w.combat.combat.attack(g!.id, wolf.id, 'light');
    w.run(TICK * 4);
  }
  assert.ok(!wolf.alive, 'the watch killed it');
  assert.equal(w.app.economy.balance(h), goldBefore, 'no loot for the hero');
  assert.ok(w.app.sink.events.some((e) => e.type === 'kill_by_watch' && e.heroId === h), "counted as the watch's kill");
  assert.ok(!w.app.sink.events.some((e) => e.type === 'kill' && e.heroId === h), 'no kill credit');
  assert.ok(TOWN_WATCH.heroShareForLoot > 0);
});

test('a drawn weapon out of town gets no warning; in town it does', () => {
  const w = world();
  const h = w.hero({ x: 50, y: 150 });
  w.combat.law!.setWeaponOut(h, true);
  w.run(3000);
  assert.ok(w.guards().every((g) => g.targetId !== h), 'nobody came over');
  w.combat.placeHero(h, { x: 50, y: 50, z: 0 });
  w.run(3000);
  assert.ok(w.guards().some((g) => g.targetId === h), 'a guard came to warn');
});

test('dev spawns reach the combat sim through the town layer', () => {
  const w = world({}, { monsters: true });
  const h = w.hero({ x: 50, y: 30 });
  const r = w.sim.devSpawn!(h, 'CREATURE_WOLF', 10);
  assert.ok('id' in r, JSON.stringify(r));
});

test('dev despawns reach the inner sim through the town layer', () => {
  const w = world();
  const calls: string[] = [];
  (w.combat as unknown as { devDespawn: (h: number, what: string) => object }).devDespawn = (_h, what) => (calls.push(what), { removed: ['CREATURE_WOLF'] });
  const r = w.sim.devDespawn!(1, 'nearest');
  assert.deepEqual(r, { removed: ['CREATURE_WOLF'] });
  assert.deepEqual(calls, ['nearest']);
});
