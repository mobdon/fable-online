import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CombatSim, BUTTONS, DEATH } from '../src/world/combat-sim.js';
import { FALLBACK_DATA } from '../src/data/game-data.js';
import { MemorySink } from '../src/analytics/events.js';
import { monsterAi } from '../src/world/ai/combat-ai.js';
import { Anim } from '../src/world/ai/types.js';
import { BALANCE } from '../src/combat/rules.js';

const TICK = 50;
const BANDIT = 'CREATURE_BANDIT_GRUNT_LEVEL1';

function arena(events = new MemorySink()) {
  const sim = new CombatSim({ name: 'TestRegion', uid: 1, sea: false, neighbours: [] }, 'TestRegion#1', {
    data: FALLBACK_DATA,
    spawns: { regions: { TestRegion: { creatures: [], generators: [] } } },
    events,
    ai: monsterAi(),
    spawnOptions: { rng: () => 0.5 },
  });
  sim.addHero(1, { x: 0, y: 0, z: 0 });
  return sim;
}
const hero = (sim: CombatSim) => sim.combat.get(1)!;
const bandit = (sim: CombatSim) => [...sim.spawns.creatures()][0]!;
const press = (sim: CombatSim, yaw = 0) => {
  sim.applyInput(1, { seq: 0, moveX: 0, moveY: 0, buttons: BUTTONS.ATTACK, cameraYaw: yaw });
  sim.applyInput(1, { seq: 0, moveX: 0, moveY: 0, buttons: 0, cameraYaw: yaw });
};

test('a test spawn comes straight for the hero who made it', () => {
  const sim = arena();
  const r = sim.devSpawn(1, BANDIT, 15);
  assert.ok('id' in r);
  const b = bandit(sim);
  let t = 0;
  for (; t < 6000 && b.anim !== Anim.Attack; t += TICK) sim.step(TICK);
  assert.ok(t < 4500, `first swing after ${t} ms`);
  assert.ok(Math.hypot(b.pos.x, b.pos.y) < 3);
});

test('a test bandit chases the hero, lands hits within seconds, each taking the share of health it takes in Fable', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 12);
  const h = hero(sim);
  let t = 0;
  for (; t < 10_000 && h.health === h.maxHealth; t += TICK) sim.step(TICK);
  assert.ok(t < 8000, `first hit after ${t} ms`);
  // Fable units: weapon damage times the creature's power, against a 35-health starting hero.
  const def = FALLBACK_DATA.creatures[BANDIT]!;
  const fableShare = (FALLBACK_DATA.items[def.weapon!]!.weapon!.damage * def.attackScale) / 35;
  const share = (h.maxHealth - h.health) / h.maxHealth;
  assert.ok(Math.abs(share - fableShare) <= 0.5 / h.maxHealth + 1e-9, `one hit took ${share}, Fable's is ${fableShare}`);
});

test('a bandit winds up before each hit, swings in short flurries and pauses between them', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 3);
  const b = bandit(sim);
  const hits: number[] = [];
  let swingStart = -1;
  const windups: number[] = [];
  let last = hero(sim).health;
  for (let t = 0; t < 30_000; t += TICK) {
    const before = b.anim;
    sim.step(TICK);
    if (b.anim === Anim.Attack && before !== Anim.Attack) swingStart = t;
    if (hero(sim).health < last) {
      hits.push(t);
      if (swingStart >= 0) windups.push(t - swingStart);
      last = hero(sim).health;
    }
  }
  assert.ok(hits.length >= 6, `only ${hits.length} hits in 30 s`);
  assert.ok(hits.length <= 30, `${hits.length} hits in 30 s is a blender`);
  assert.ok(windups.every((w) => w >= 300), `a hit landed without a visible wind-up: ${windups}`);
  // Never more than two hits inside any 1.2 s window: flurries are one or two swings.
  for (let i = 2; i < hits.length; i++) assert.ok(hits[i]! - hits[i - 2]! > 1200, `three hits in a row at ${hits.slice(i - 2, i + 1)}`);
  assert.ok(hero(sim).health > 0, 'a passive hero survives 30 s against one grunt');
});

test('stepping back out of reach during the wind-up dodges the swing', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  for (let t = 0; t < 5000 && b.anim !== Anim.Attack; t += TICK) sim.step(TICK);
  assert.equal(b.anim, Anim.Attack);
  const hp = hero(sim).health;
  sim.placeHero(1, { x: 0, y: -8, z: 0 });
  for (let t = 0; t < 600; t += TICK) sim.step(TICK);
  assert.equal(hero(sim).health, hp);
});

test('hitting a bandit during its wind-up interrupts the swing', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  for (let t = 0; t < 5000 && b.anim !== Anim.Attack; t += TICK) sim.step(TICK);
  const hp = hero(sim).health;
  b.blocking = false;
  press(sim, Math.atan2(-(b.pos.x - 0), b.pos.y - 0));
  assert.ok(b.health < b.maxHealth, 'the hit landed');
  for (let t = 0; t < 600; t += TICK) sim.step(TICK);
  assert.equal(hero(sim).health, hp, 'the interrupted swing did nothing');
});

test("a bandit whose fight ends walks home with its wounds; only a leash reset heals", () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 3);
  const b = bandit(sim);
  for (let t = 0; t < 1000; t += TICK) sim.step(TICK);
  b.health = 12;
  sim.combat.get(1)!.alive = false; // the hero falls
  for (let t = 0; t < 15_000; t += TICK) sim.step(TICK);
  assert.equal(b.health, 12);
});

test('hero swings: a press late in the last swing is kept, and aim is as forgiving as Fable', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  b.maxHealth = b.health = 1000;
  // The bandit is ahead (+y); the hero faces 80 degrees off to the side and still connects.
  const yaw = (80 * Math.PI) / 180;
  press(sim, yaw);
  const afterFirst = b.health;
  assert.ok(afterFirst < 1000, 'aim snapped onto the bandit');
  sim.step(Math.max(TICK, BALANCE.cooldownMs.unarmed! - BALANCE.swingBufferMs + 100)); // inside the buffer
  b.blocking = false;
  press(sim, yaw);
  assert.equal(b.health, afterFirst, 'not yet: the swing is queued');
  for (let t = 0; t < 600; t += TICK) {
    b.blocking = false;
    sim.step(TICK);
  }
  assert.ok(b.health < afterFirst, 'the queued swing landed');
});

test("a circling bandit stays inside a bare-handed hero's reach", () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 3);
  const b = bandit(sim);
  b.maxHealth = b.health = 10_000;
  const reach = BALANCE.reach.unarmed! + BALANCE.bodyRadius + BALANCE.heroReachSlack;
  let furthest = 0;
  for (let t = 0; t < 20_000; t += TICK) {
    sim.step(TICK);
    if (t > 3000) furthest = Math.max(furthest, Math.hypot(b.pos.x, b.pos.y));
  }
  assert.ok(furthest <= reach, `circled out to ${furthest.toFixed(2)} m, reach is ${reach} m`);
});

test('a test monster killed by its hero is logged with the damage that did it', () => {
  const events = new MemorySink();
  const sim = arena(events);
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  b.health = 1;
  b.blocking = false;
  press(sim, Math.atan2(-b.pos.x, b.pos.y));
  for (let t = 0; t < 200; t += TICK) sim.step(TICK);
  const all = events.events as { type: string; heroId?: number; killer?: string | null; heroDamage?: number }[];
  assert.ok(all.some((e) => e.type === 'dev_spawn' && e.heroId === 1));
  const kill = all.find((e) => e.type === 'dev_kill');
  assert.ok(kill, 'the kill was logged');
  assert.equal(kill!.heroId, 1);
  assert.equal(kill!.killer, 'hero:1');
  assert.ok(kill!.heroDamage! > 0);
});

test('a flourish being charged when the hero falls is gone when they get up', () => {
  const sim = arena();
  sim.addHero(2, { x: 0, y: 1, z: 0 });
  const h = sim.hero(1)!;
  const other = sim.hero(2)!;
  h.pvp = other.pvp = true;
  sim.applyInput(1, { seq: 0, moveX: 0, moveY: 0, buttons: BUTTONS.ATTACK, cameraYaw: 0 });
  for (let t = 0; t < 300; t += TICK) sim.step(TICK);
  h.health = 1;
  other.yaw = Math.PI;
  assert.ok(sim.combat.attack(2, 1, 'light').ok);
  assert.equal(h.alive, false);
  for (let t = 0; t < DEATH.respawnMs + 2000; t += TICK) sim.step(TICK);
  assert.equal(h.alive, true);
  assert.equal(h.chargeMs, 0, 'no charge carried through death');
});

test('hero swings: a press while flinching is held and fires when the flinch ends, shown only then', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  b.maxHealth = b.health = 1000;
  const h = hero(sim);
  const yaw = Math.atan2(-b.pos.x, b.pos.y);
  h.stunnedMs = BALANCE.flinchMs;
  press(sim, yaw);
  sim.step(TICK);
  assert.notEqual(h.anim, Anim.Attack, 'no swing shown while it cannot land');
  assert.equal(b.health, 1000);
  let swung = false;
  for (let t = 0; t < BALANCE.flinchMs + 100; t += TICK) {
    b.blocking = false;
    sim.step(TICK);
    swung ||= h.anim === Anim.Attack;
  }
  assert.ok(b.health < 1000, 'the held swing landed');
  assert.ok(swung, 'and was shown when it fired');
});

test('hero swings: only one press is held, and one pressed too early is dropped without a swing anim', () => {
  const sim = arena();
  sim.devSpawn(1, BANDIT, 2);
  const b = bandit(sim);
  b.maxHealth = b.health = 1000;
  const h = hero(sim);
  const yaw = Math.atan2(-b.pos.x, b.pos.y);
  // Knocked down for longer than a press is held: dropped, and no swing shown.
  h.stunnedMs = 2000;
  press(sim, yaw);
  sim.step(TICK);
  assert.notEqual(h.anim, Anim.Attack);
  h.stunnedMs = 0;
  for (let t = 0; t < 1000; t += TICK) sim.step(TICK);
  assert.equal(b.health, 1000, 'the early press never fired');
  // Spam during one cooldown: at most one extra swing.
  b.blocking = false;
  press(sim, yaw);
  const first = 1000 - b.health;
  assert.ok(first > 0);
  for (let i = 0; i < 5; i++) press(sim, yaw);
  for (let t = 0; t < 2000; t += TICK) {
    b.blocking = false;
    b.stunnedMs = 0;
    sim.step(TICK);
  }
  assert.ok(1000 - b.health <= first * 2 + 1, `took ${1000 - b.health}, one hit is ${first}`);
});
