import { test } from 'node:test';
import assert from 'node:assert/strict';
import { NavSim, StubCombat } from '../src/world/nav-sim.js';
import { profileFor } from '../src/world/ai/profiles.js';
import { Anim } from '../src/world/ai/types.js';
import { asciiNav, field } from './nav-fixture.js';

const TICK = 50;
const run = (sim: NavSim, ms: number) => {
  for (let t = 0; t < ms; t += TICK) sim.step(TICK);
};
const dist = (a: { x: number; y: number }, b: { x: number; y: number }) => Math.hypot(a.x - b.x, a.y - b.y);
const combat = (sim: NavSim) => (sim as unknown as { world: { combat: StubCombat } }).world.combat;

test('def names pick the right family, and game.bin stats override the defaults', () => {
  assert.equal(profileFor({ defName: 'CREATURE_BANDIT_GRUNT_LEVEL1' }).family, 'bandit');
  assert.equal(profileFor({ defName: 'CREATURE_BANDIT_ARCHER' }).family, 'banditArcher');
  assert.equal(profileFor({ defName: 'CREATURE_HOBBE_LEVEL2' }).family, 'hobbe');
  assert.equal(profileFor({ defName: 'CREATURE_WOLF_WHITE' }).family, 'wolf');
  assert.equal(profileFor({ defName: 'CREATURE_BALVERINE_WHITE' }).family, 'balverine');
  assert.equal(profileFor({ defName: 'CREATURE_NYMPH' }).family, 'default');
  const p = profileFor({ defName: 'CREATURE_HOBBE', stats: { runSpeed: 9, flees: false } });
  assert.equal(p.runSpeed, 9);
  assert.equal(p.fleeAt, 0);
});

test('an idle creature wanders around its spawn point and stays on the ground', () => {
  const sim = new NavSim(field());
  const wolf = sim.spawn('CREATURE_WOLF', { x: 20, y: 20, z: 0 });
  let moved = false;
  for (let t = 0; t < 60_000; t += TICK) {
    sim.step(TICK);
    if (dist(wolf.pos, { x: 20, y: 20 }) > 1) moved = true;
    assert.ok(dist(wolf.pos, { x: 20, y: 20 }) <= profileFor(wolf).wanderRadius + 0.5, 'stays near home');
    assert.ok(sim.nav.walkable(wolf.pos.x, wolf.pos.y));
  }
  assert.ok(moved, 'it wandered');
});

test('a bandit notices a hero in front of it, pauses, chases and swings', () => {
  const sim = new NavSim(field());
  const bandit = sim.spawn('CREATURE_BANDIT_GRUNT', { x: 20, y: 10, z: 0 }, { yaw: 0 }); // facing +y
  sim.addHero(1, { x: 20.5, y: 20.5, z: 0 });
  run(sim, 400);
  assert.equal(bandit.targetId, 1);
  assert.equal(sim.ai.state(bandit.id), 'alert');
  run(sim, 4000);
  assert.ok(dist(bandit.pos, sim.position(1)!) <= profileFor(bandit).attackRange + 0.1, 'closed the distance');
  assert.ok(combat(sim).log.some((s) => s.attacker === bandit.id && s.target === 1), 'swung at the hero');
});

test('a hero behind a creature and outside hearing range goes unnoticed', () => {
  const sim = new NavSim(field());
  const hobbe = sim.spawn('CREATURE_HOBBE', { x: 20, y: 20, z: 0 }, { yaw: 0 }); // facing +y
  sim.addHero(1, { x: 20, y: 11, z: 0 }); // 9 m straight behind
  run(sim, 1000);
  assert.equal(hobbe.targetId, null);
});

test('being hit makes a creature fight back at once and calls its pack', () => {
  const sim = new NavSim(field());
  const a = sim.spawn('CREATURE_WOLF', { x: 10, y: 10, z: 0 }, { yaw: Math.PI });
  const b = sim.spawn('CREATURE_WOLF', { x: 13, y: 10, z: 0 }, { yaw: Math.PI });
  const far = sim.spawn('CREATURE_WOLF', { x: 38, y: 38, z: 0 }, { yaw: Math.PI });
  sim.addHero(7, { x: 10, y: 30, z: 0 }); // 20 m away, behind them
  sim.ai.onDamaged(a.id, 7, 5);
  assert.equal(sim.ai.state(a.id), 'chase');
  assert.equal(b.targetId, 7, 'pack-mate joins');
  assert.equal(far.targetId, null, 'a wolf across the map does not');
});

test('a Hobbe at low health runs away once, then comes back if cornered or goes home', () => {
  const sim = new NavSim(field());
  const hobbe = sim.spawn('CREATURE_HOBBE', { x: 20, y: 20, z: 0 });
  sim.addHero(1, { x: 21, y: 21, z: 0 });
  sim.ai.onDamaged(hobbe.id, 1, 10);
  hobbe.health = 20; // 20% < 25%
  sim.ai.onDamaged(hobbe.id, 1, 1);
  assert.equal(sim.ai.state(hobbe.id), 'flee');
  const before = dist(hobbe.pos, sim.position(1)!);
  run(sim, 2000);
  assert.ok(dist(hobbe.pos, sim.position(1)!) > before + 5, 'put distance between them');
  run(sim, 3000);
  assert.notEqual(sim.ai.state(hobbe.id), 'flee', 'only flees for a while');
});

test('leash: pulled too far from home it gives up, runs home ignoring hits, and heals', () => {
  const sim = new NavSim(field(120));
  const bandit = sim.spawn('CREATURE_BANDIT', { x: 10, y: 10, z: 0 });
  sim.addHero(1, { x: 12, y: 12, z: 0 });
  sim.ai.onDamaged(bandit.id, 1, 30);
  bandit.health = 70;
  // Hero runs away in a straight line, faster than a bandit can't, far past the leash.
  for (let t = 0; t < 30_000 && sim.ai.state(bandit.id) !== 'return'; t += TICK) {
    sim.applyInput(1, { seq: t, moveX: 0, moveY: 1, buttons: 0, cameraYaw: -Math.PI / 4 });
    sim.step(TICK);
  }
  assert.equal(sim.ai.state(bandit.id), 'return');
  sim.ai.onDamaged(bandit.id, 1, 50);
  assert.equal(sim.ai.state(bandit.id), 'return', 'evading ignores damage');
  run(sim, 20_000);
  assert.equal(sim.ai.state(bandit.id), 'idle');
  assert.equal(bandit.health, bandit.maxHealth);
  assert.ok(dist(bandit.pos, { x: 10, y: 10 }) <= profileFor(bandit).wanderRadius + 0.5, "back on its patch");
});

test('threat: the hero doing the damage pulls a creature off the closer one, without ping-pong', () => {
  const sim = new NavSim(field());
  const bal = sim.spawn('CREATURE_BALVERINE', { x: 20, y: 20, z: 0 });
  sim.addHero(1, { x: 21.5, y: 20, z: 0 }); // tank, right next to it
  sim.addHero(2, { x: 26, y: 20, z: 0 }); // caster, further away
  sim.ai.onDamaged(bal.id, 1, 2);
  run(sim, 400);
  assert.equal(bal.targetId, 1);
  sim.ai.onDamaged(bal.id, 2, 3); // a little more than the tank: not enough to switch
  run(sim, 400);
  assert.equal(bal.targetId, 1);
  sim.ai.onDamaged(bal.id, 2, 40); // a big hit: switch
  run(sim, 400);
  assert.equal(bal.targetId, 2);
});

test('a pack surrounds a hero instead of stacking on one spot', () => {
  const sim = new NavSim(field());
  const wolves = [0, 1, 2, 3].map((i) => sim.spawn('CREATURE_WOLF', { x: 14 + i, y: 10, z: 0 }));
  sim.addHero(1, { x: 20, y: 20, z: 0 });
  sim.ai.onDamaged(wolves[0]!.id, 1, 1);
  run(sim, 6000);
  for (let i = 0; i < wolves.length; i++)
    for (let j = i + 1; j < wolves.length; j++) assert.ok(dist(wolves[i]!.pos, wolves[j]!.pos) > 0.4, `wolves ${i} and ${j} overlap`);
});

test('creatures path around walls to reach a hero', () => {
  const nav = asciiNav([
    '..............', // y = 6
    '..............',
    '.############.',
    '..............', // y = 3
    '..............',
    '..............',
    '..............',
  ]);
  const sim = new NavSim(nav);
  const wolf = sim.spawn('CREATURE_WOLF', { x: 6.5, y: 1.5, z: 0 });
  sim.addHero(1, { x: 6.5, y: 5.5, z: 0 });
  sim.ai.onDamaged(wolf.id, 1, 1);
  for (let t = 0; t < 6000; t += TICK) {
    sim.step(TICK);
    assert.ok(nav.walkable(wolf.pos.x, wolf.pos.y), `wolf at ${wolf.pos.x},${wolf.pos.y} is in a wall`);
  }
  assert.ok(wolf.pos.y > 4, 'got round the wall');
});

test('heroes cannot walk through walls on the navmesh, and stand on the ground', () => {
  const nav = asciiNav(['.....', '#####', '.....'], () => 3);
  const sim = new NavSim(nav);
  sim.addHero(1, { x: 2.5, y: 0.5, z: 99 });
  assert.equal(sim.position(1)!.z, 3);
  sim.applyInput(1, { seq: 1, moveX: 0, moveY: 1, buttons: 0, cameraYaw: 0 }); // walk +y into the wall
  run(sim, 2000);
  assert.ok(sim.position(1)!.y < 1, 'stopped at the wall');
});

test('dead creatures stop, and their deaths can break the rest of the group', () => {
  const sim = new NavSim(field());
  const gang = [0, 1, 2, 3].map((i) => sim.spawn('CREATURE_BANDIT', { x: 15 + i, y: 15, z: 0 }));
  sim.addHero(1, { x: 16, y: 18, z: 0 });
  sim.ai.onDamaged(gang[0]!.id, 1, 1);
  run(sim, 500);
  for (const g of gang.slice(0, 3)) {
    g.alive = false;
    g.health = 0;
  }
  run(sim, 300);
  for (const g of gang.slice(0, 3)) assert.equal(sim.ai.state(g.id), 'dead');
  assert.equal(gang[0]!.anim, Anim.Death);
  assert.equal(sim.ai.state(gang[3]!.id), 'flee', 'last bandit breaks after three deaths');
});

test('snapshot carries creatures with their def name for the client', () => {
  const sim = new NavSim(field());
  sim.spawn('CREATURE_HOBBE', { x: 5, y: 5, z: 0 });
  sim.addHero(1, { x: 30, y: 30, z: 0 });
  const snap = sim.snapshot();
  assert.equal(snap.length, 2);
  const c = snap.find((e) => e.id >= 1_000_000)!;
  assert.equal(c.def, 'CREATURE_HOBBE');
});

test('in a combat channel the AI drives spawned monsters: a bandit walks up and hits the hero', async () => {
  const { CombatSim } = await import('../src/world/combat-sim.js');
  const { FALLBACK_DATA } = await import('../src/data/game-data.js');
  const { MemorySink } = await import('../src/analytics/events.js');
  const { monsterAi } = await import('../src/world/ai/combat-ai.js');
  const table = { creatures: [{ uid: '1', def: 'CREATURE_BANDIT_GRUNT_LEVEL1', x: 0, y: 10, z: 0, yaw: Math.PI, script: null }], generators: [] };
  const sim = new CombatSim({ name: 'TestRegion', uid: 1, sea: false, neighbours: [] }, 'TestRegion#1', {
    data: FALLBACK_DATA,
    spawns: { regions: { TestRegion: table } },
    events: new MemorySink(),
    ai: monsterAi(),
    spawnOptions: { rng: () => 0.5 },
  });
  sim.addHero(1, { x: 0, y: 0, z: 0 });
  const bandit = [...sim.spawns.creatures()][0]!;
  const startHp = sim.combat.get(1)!.health;
  // The bandit faces the hero (yaw pi = -y) 10 m away: inside its sight.
  for (let t = 0; t < 8000; t += 50) sim.step(50);
  assert.equal(bandit.targetId, 1);
  assert.ok(Math.hypot(bandit.pos.x, bandit.pos.y) < 3, `bandit at ${bandit.pos.x},${bandit.pos.y} never closed in`);
  assert.ok(sim.combat.get(1)!.health < startHp, 'the hero took hits');
});
