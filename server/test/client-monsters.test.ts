import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { type SpawnTable } from '../src/world/spawns.js';
import { ClientKillLedger, DEFAULT_CLIENT_KILL_OPTIONS } from '../src/world/client-kills.js';
import { CombatSim } from '../src/world/combat-sim.js';
import { LootService } from '../src/economy/loot.js';
import { decode } from '../src/net/protocol.js';
import { makeHero, testApp } from './helpers.js';

const TABLE: SpawnTable = {
  creatures: [
    { uid: '1', def: 'CREATURE_BANDIT_GRUNT_LEVEL1', x: 0, y: 3, z: 0, yaw: 0, script: null },
    { uid: '2', def: 'CREATURE_TRADER_TEST', x: 5, y: 5, z: 0, yaw: 0, script: null },
  ],
  generators: [{ uid: '3', x: 100, y: 100, z: 0, families: ['WOLVES_TEST'], radius: 5, triggerRadius: 0, activeLimit: 2, totalLimit: 3, script: null }],
};
const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };
const O = { ...DEFAULT_CLIENT_KILL_OPTIONS, respawnMs: 1000, generatorRespawnMs: 1000 };

test('client kills: a placed monster pays once per respawn, a generator its active count; extras pay nothing', () => {
  const l = new ClientKillLedger(TABLE, D, O);
  const hero = { x: 0, y: 0, z: 0 };
  assert.deepEqual(l.claim('CREATURE_BANDIT_GRUNT_LEVEL1', { x: 2, y: 4, z: 0 }, hero, 0), { ok: true, slot: '1' });
  assert.deepEqual(l.claim('CREATURE_BANDIT_GRUNT_LEVEL1', { x: 2, y: 4, z: 0 }, hero, 500), { ok: false, reason: 'spent' });
  assert.equal(l.claim('CREATURE_BANDIT_GRUNT_LEVEL1', { x: 2, y: 4, z: 0 }, hero, 1200).ok, true, 'respawned');
  // Not a monster the level places, or nowhere near its spot, or the hero far away.
  assert.deepEqual(l.claim('CREATURE_EARTH_TROLL', { x: 0, y: 3, z: 0 }, hero, 0), { ok: false, reason: 'unknown_def' });
  assert.deepEqual(l.claim('CREATURE_TRADER_TEST', { x: 5, y: 5, z: 0 }, hero, 0), { ok: false, reason: 'unknown_def' });
  assert.deepEqual(l.claim('CREATURE_BANDIT_GRUNT_LEVEL1', { x: 300, y: 3, z: 0 }, { x: 300, y: 0, z: 0 }, 5000), { ok: false, reason: 'no_spot' });
  assert.deepEqual(l.claim('CREATURE_BANDIT_GRUNT_LEVEL1', { x: 0, y: 3, z: 0 }, { x: 200, y: 0, z: 0 }, 5000), { ok: false, reason: 'hero_far' });
  const camp = { x: 100, y: 100, z: 0 };
  assert.equal(l.claim('CREATURE_WOLF', camp, camp, 0).ok, true);
  assert.equal(l.claim('CREATURE_WOLF', camp, camp, 10).ok, true);
  assert.deepEqual(l.claim('CREATURE_WOLF', camp, camp, 20), { ok: false, reason: 'spent' });
});

test('client monsters: the sim spawns none, pays a reported kill once, and takes reported hurts', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const loot = new LootService(app.economy, D, {}, () => 0.1);
  const sim = new CombatSim(REGION, 'TestRegion#1', {
    data: D,
    spawns: { regions: { TestRegion: TABLE } },
    events: app.sink,
    economy: app.economy,
    loot,
    progress: app.progress,
    clientMonsters: true,
  });
  sim.addHero(hero.id, { x: 0, y: 0, z: 0 });
  assert.equal([...sim.spawns.creatures()].length, 0, 'no server monsters');
  assert.equal(sim.clientKill(hero.id, 'CREATURE_BANDIT_GRUNT_LEVEL1', { x: 1, y: 3, z: 0 }).ok, true);
  assert.equal(app.economy.balance(hero.id), D.creatures.CREATURE_BANDIT_GRUNT_LEVEL1!.reward2);
  assert.equal(app.progress.get(hero.id).kills, 1);
  assert.equal(sim.clientKill(hero.id, 'CREATURE_BANDIT_GRUNT_LEVEL1', { x: 1, y: 3, z: 0 }).ok, false, 'one payout per spawn');
  assert.equal(app.economy.balance(hero.id), D.creatures.CREATURE_BANDIT_GRUNT_LEVEL1!.reward2);
  const before = sim.combat.get(hero.id)!.health;
  assert.ok(sim.clientHurt(hero.id, 0.25) > 0);
  assert.ok(sim.combat.get(hero.id)!.health < before);
  assert.equal(app.economy.audit().ok, true);
  app.close();
});

test('protocol: kill and hurt are checked', () => {
  assert.deepEqual(decode('{"t":"kill","def":"CREATURE_WOLF","x":1,"y":2,"z":3}'), { t: 'kill', def: 'CREATURE_WOLF', x: 1, y: 2, z: 3 });
  assert.throws(() => decode('{"t":"kill","def":"wolf; drop","x":1,"y":2,"z":3}'));
  assert.deepEqual(decode('{"t":"hurt","share":0.1}'), { t: 'hurt', share: 0.1 });
  assert.throws(() => decode('{"t":"hurt","share":5}'));
});
