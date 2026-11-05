import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { MemorySink } from '../src/analytics/events.js';
import { CombatSim, DEATH } from '../src/world/combat-sim.js';
import type { Vec3 } from '../src/world/sim.js';

const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };

function fallen(respawnOk: boolean) {
  const calls: { heroId: number; fell: Vec3; serverPos: Vec3 | undefined }[] = [];
  const moved: { heroId: number; at: Vec3 }[] = [];
  const sim: CombatSim = new CombatSim(REGION, 'TestRegion#d', {
    data: D,
    spawns: { regions: {} },
    events: new MemorySink(),
    respawn: (heroId, fell) => {
      calls.push({ heroId, fell: { ...fell }, serverPos: sim.position(heroId) });
      return respawnOk;
    },
    moved: (heroId, at) => void moved.push({ heroId, at: { ...at } }),
  });
  sim.addHero(1, { x: 50, y: 0, z: 0 });
  sim.addHero(2, { x: 10, y: 11, z: 0 });
  const h = sim.hero(1)!;
  h.pos = { x: 10, y: 10, z: 0 };
  h.pvp = sim.hero(2)!.pvp = true;
  h.health = 1;
  sim.hero(2)!.yaw = Math.PI;
  const r = sim.combat.attack(2, 1, 'light');
  assert.ok(r.ok && r.hit && r.killed);
  return { sim, calls, moved, h };
}

test('respawn picks its gate from where the hero fell, not from where they entered the channel', async () => {
  const { sim, calls, moved } = fallen(true);
  sim.step(DEATH.respawnMs + 10);
  await Promise.resolve();
  assert.equal(calls.length, 1);
  assert.deepEqual(calls[0]!.fell, { x: 10, y: 10, z: 0 });
  assert.deepEqual(calls[0]!.serverPos, { x: 10, y: 10, z: 0 });
  assert.equal(moved.length, 0);
});

test('with no gate to go to the hero gets up at the entry point and the client is told', async () => {
  const { sim, moved, h } = fallen(false);
  sim.step(DEATH.respawnMs + 10);
  await Promise.resolve();
  assert.equal(h.alive, true);
  assert.deepEqual(h.pos, { x: 50, y: 0, z: 0 });
  assert.deepEqual(moved, [{ heroId: 1, at: { x: 50, y: 0, z: 0 } }]);
});
