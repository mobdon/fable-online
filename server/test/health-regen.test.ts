import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { MemorySink } from '../src/analytics/events.js';
import { CombatSim, HEALTH_REGEN } from '../src/world/combat-sim.js';

const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };

test('health comes back slowly once the hero has been out of combat for a while', () => {
  const sim = new CombatSim(REGION, 'TestRegion#r', { data: D, spawns: { regions: {} }, events: new MemorySink() });
  sim.addHero(1, { x: 0, y: 0, z: 0 });
  const h = sim.hero(1)!;
  h.health = 50;
  for (let t = 0; t < HEALTH_REGEN.calmMs - 100; t += 50) sim.step(50);
  assert.equal(h.health, 50, 'nothing during the wait');
  for (let t = 0; t < 1100; t += 50) sim.step(50);
  assert.ok(h.health > 50 && h.health < 50 + h.maxHealth * HEALTH_REGEN.sharePerSec * 1.5, `health ${h.health}`);
  for (let t = 0; t < 200_000; t += 1000) sim.step(1000);
  assert.equal(h.health, h.maxHealth, 'capped at max');
});

test('taking or dealing a hit restarts the wait', () => {
  const sim = new CombatSim(REGION, 'TestRegion#r', { data: D, spawns: { regions: {} }, events: new MemorySink() });
  sim.addHero(1, { x: 0, y: 0, z: 0 });
  sim.addHero(2, { x: 0, y: 1, z: 0 });
  const h = sim.hero(1)!;
  h.pvp = true;
  sim.hero(2)!.pvp = true;
  h.health = 50;
  for (let t = 0; t < HEALTH_REGEN.calmMs - 500; t += 50) sim.step(50);
  const r = sim.combat.attack(1, 2, 'light');
  assert.ok(r.ok && r.hit, 'the hit lands');
  for (let t = 0; t < HEALTH_REGEN.calmMs - 500; t += 50) sim.step(50);
  assert.equal(h.health, 50);
});
