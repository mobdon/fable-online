import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { CombatSim, GUARD_RESPAWN_MS } from '../src/world/combat-sim.js';
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
function world(over: object = {}) {
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
    (region, channelId) => new CombatSim(region, channelId, { data: D, spawns: { regions: {} }, events: app.sink, economy: app.economy, law: { law, events: app.sink, rng: () => 0 } }),
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

test('the town layer posts guards at each village, and they show in the snapshot', () => {
  const w = world();
  const g = w.guards();
  assert.equal(g.length, 2);
  for (const c of g) assert.ok(Math.hypot(c.pos.x - 50, c.pos.y - 50) <= 3.1, 'near the village centre');
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
