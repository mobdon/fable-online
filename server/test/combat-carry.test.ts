import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { MemorySink } from '../src/analytics/events.js';
import { CARRY_EXPIRE_MS, combatSimFactory, type CombatSim } from '../src/world/combat-sim.js';

const region = (name: string) => ({ name, uid: 1, sea: false, neighbours: [] });
const at = { x: 0, y: 0, z: 0 };
const services = () => ({ data: D, spawns: { regions: {} }, events: new MemorySink() });

test('health and Will carry over to the next channel', () => {
  const make = combatSimFactory(services());
  const a = make(region('A'), 'A#1') as CombatSim;
  const b = make(region('B'), 'B#1') as CombatSim;
  a.addHero(1, at);
  const h = a.hero(1)!;
  h.health = h.maxHealth * 0.3;
  h.will = h.maxWill * 0.5;
  a.removeHero(1);
  b.addHero(1, at);
  const h2 = b.hero(1)!;
  assert.equal(h2.health, Math.round(h2.maxHealth * 0.3));
  assert.equal(h2.will, Math.round(h2.maxWill * 0.5));
  // The entry is used once: a later join starts from what the next leave records.
  b.removeHero(1);
  a.addHero(1, at);
  assert.equal(a.hero(1)!.health, Math.round(h2.maxHealth * 0.3));
});

test('a hero who leaves while down arrives at the respawn share of health', () => {
  const make = combatSimFactory(services());
  const a = make(region('A'), 'A#1') as CombatSim;
  a.addHero(1, at);
  const h = a.hero(1)!;
  h.alive = false;
  h.health = 0;
  a.removeHero(1);
  a.addHero(1, at);
  const h2 = a.hero(1)!;
  assert.ok(h2.alive);
  assert.equal(h2.health, Math.round(h2.maxHealth * 0.5));
});

test('separate apps keep separate records, and old records expire', () => {
  const s = services();
  const make = combatSimFactory(s);
  const other = combatSimFactory(services());
  const a = make(region('A'), 'A#1') as CombatSim;
  a.addHero(1, at);
  a.hero(1)!.health = 10;
  a.removeHero(1);
  const o = other(region('A'), 'A#1') as CombatSim;
  o.addHero(1, at);
  assert.equal(o.hero(1)!.health, o.hero(1)!.maxHealth, 'another app does not see the record');

  const realNow = Date.now;
  try {
    const t0 = realNow();
    Date.now = () => t0 + CARRY_EXPIRE_MS + 1;
    a.addHero(1, at);
    assert.equal(a.hero(1)!.health, a.hero(1)!.maxHealth, 'an expired record is ignored');
  } finally {
    Date.now = realNow;
  }
});
