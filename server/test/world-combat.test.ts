import { test } from 'node:test';
import assert from 'node:assert/strict';
import { CombatSystem } from '../src/combat/combat.js';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { SpawnManager, type SpawnTable } from '../src/world/spawns.js';
import { BUTTONS, CombatSim, DEATH, SPELL_SHIFT } from '../src/world/combat-sim.js';
import { MemorySink } from '../src/analytics/events.js';
import { LootService } from '../src/economy/loot.js';
import { Shops } from '../src/economy/shops.js';
import type { Creature } from '../src/world/entities.js';
import { key, makeHero, testApp } from './helpers.js';

const TABLE: SpawnTable = {
  creatures: [
    { uid: '1', def: 'CREATURE_BANDIT_GRUNT_LEVEL1', x: 0, y: 3, z: 0, yaw: 0, script: null },
    { uid: '2', def: 'CREATURE_TRADER_TEST', x: 5, y: 5, z: 0, yaw: 0, script: null }, // not hostile: never spawned
  ],
  generators: [
    { uid: '3', x: 100, y: 100, z: 0, families: ['WOLVES_TEST', 'NOT_A_FAMILY'], radius: 5, triggerRadius: 0, activeLimit: 2, totalLimit: 3, script: null },
  ],
};
const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };

test('spawns: hostile placed creatures and generators fill up, respawn after death, respect total limits', () => {
  const combat = new CombatSystem(D);
  const spawned: Creature[] = [];
  const sm = new SpawnManager(TABLE, D, combat, { onSpawn: (c) => spawned.push(c) }, { rng: () => 0.5, respawnMs: 1000, generatorRespawnMs: 500, corpseMs: 100 });
  assert.equal(sm.pointCount, 2, 'the trader is skipped');
  sm.start();
  assert.equal([...sm.creatures()].length, 3);
  assert.equal(spawned.length, 3);
  for (const c of sm.creatures()) assert.ok(c.id >= 1_000_000);
  const wolves = [...sm.creatures()].filter((c) => c.defName === 'CREATURE_WOLF');
  for (const w of wolves) assert.ok(Math.hypot(w.pos.x - 100, w.pos.y - 100) <= 5);

  // Kill one wolf: it respawns once (total limit 3), then the generator is spent.
  const w0 = wolves[0]!;
  w0.alive = false;
  sm.died(w0.id);
  sm.step(200); // corpse removed
  assert.equal(combat.get(w0.id), undefined);
  sm.step(1000);
  assert.equal([...sm.creatures()].filter((c) => c.defName === 'CREATURE_WOLF').length, 2);
  const w1 = [...sm.creatures()].find((c) => c.defName === 'CREATURE_WOLF')!;
  w1.alive = false;
  sm.died(w1.id);
  sm.step(5000);
  assert.equal([...sm.creatures()].filter((c) => c.defName === 'CREATURE_WOLF' && c.alive).length, 1, 'total limit reached');
});

test('combat sim: a kill pays loot through the ledger and experience; replays never pay twice', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const loot = new LootService(app.economy, D, { goldPerReward: 1, byGroup: {}, byCreature: {} }, () => 0.99);
  const sim = new CombatSim(REGION, 'TestRegion#1', {
    data: D,
    spawns: { regions: { TestRegion: TABLE } },
    events: app.sink,
    economy: app.economy,
    loot,
    progress: app.progress,
    loadout: () => ({ equipped: ['OBJECT_IRON_LONGSWORD'] }),
    spawnOptions: { rng: () => 0.5 },
  });
  sim.addHero(hero.id, { x: 0, y: 0, z: 0 });
  const bandit = [...sim.spawns.creatures()].find((c) => c.defName === 'CREATURE_BANDIT_GRUNT_LEVEL1')!;
  bandit.health = 1;
  bandit.pos.y = 2; // within sword reach
  // press and release attack
  sim.applyInput(hero.id, { seq: 1, moveX: 0, moveY: 0, buttons: BUTTONS.ATTACK, cameraYaw: 0 });
  sim.step(50);
  sim.applyInput(hero.id, { seq: 2, moveX: 0, moveY: 0, buttons: 0, cameraYaw: 0 });
  assert.equal(bandit.alive, false);
  const gold = app.economy.balance(hero.id);
  assert.ok(gold > 0 && gold <= D.creatures.CREATURE_BANDIT_GRUNT_LEVEL1!.reward2);
  assert.equal(app.progress.get(hero.id).kills, 1);
  assert.ok(app.progress.get(hero.id).experience >= D.creatures.CREATURE_BANDIT_GRUNT_LEVEL1!.experience);
  // Paying the same creature again is a no-op.
  loot.award(hero.id, bandit);
  assert.equal(app.economy.balance(hero.id), gold);
  assert.ok(app.sink.events.some((e) => e.type === 'kill'));
  assert.equal(app.economy.audit().ok, true);
  app.close();
});

test('combat sim: a hero with a Resurrection Phial uses it and gets up where they fell; without one they lose a little gold', () => {
  const app = testApp();
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  app.economy.grant(a.id, 1000, 'gm_grant', key(), 'gm:1');
  app.economy.grant(b.id, 1000, 'gm_grant', key(), 'gm:1');
  app.economy.mintItem(a.id, 'OBJECT_RESURRECTION_PHIAL', 1, 'test', key(), 'gm:1');
  const sim = new CombatSim(REGION, 'TestRegion#2', { data: D, spawns: { regions: {} }, events: app.sink, economy: app.economy, progress: app.progress });
  sim.addHero(a.id, { x: 0, y: 0, z: 0 });
  sim.addHero(b.id, { x: 50, y: 0, z: 0 });
  const ha = sim.hero(a.id)!;
  const hb = sim.hero(b.id)!;
  ha.pos = { x: 10, y: 10, z: 0 };
  // Kill both with a creature hit.
  for (const h of [ha, hb]) {
    h.health = 1;
    const wolf = { id: 9_000_000 + h.id, kind: 'creature', defName: 'CREATURE_WOLF', pos: { x: h.pos.x, y: h.pos.y - 1, z: 0 }, yaw: 0, health: 20, maxHealth: 20, alive: true, faction: 'FACTION_MONSTERS', anim: 0, blocking: false, stunnedMs: 0, attackCooldownMs: 0, weapon: null, power: 2, armour: 0, targetId: null, spawnId: null, experience: 0 } as Creature;
    sim.combat.add(wolf);
    const r = sim.combat.attack(wolf.id, h.id, 'light');
    assert.ok(r.ok && r.hit && r.killed);
  }
  assert.equal(app.economy.inventory(a.id).length, 0, 'phial used up');
  assert.equal(app.economy.balance(a.id), 1000, 'no penalty with a phial');
  assert.equal(app.economy.balance(b.id), 1000 - Math.floor(1000 * DEATH.goldPenalty));
  sim.step(DEATH.phialMs + 10);
  assert.equal(ha.alive, true);
  assert.deepEqual(ha.pos, { x: 10, y: 10, z: 0 });
  assert.equal(hb.alive, false, 'still down');
  sim.step(DEATH.respawnMs);
  assert.equal(hb.alive, true);
  assert.deepEqual(hb.pos, { x: 50, y: 0, z: 0 }, 'back at the entry point');
  assert.equal(app.economy.audit().ok, true);
  app.close();
});

test('combat sim: cast button with a spell slot casts that spell', () => {
  const sink = new MemorySink();
  const sim = new CombatSim(REGION, 'TestRegion#3', { data: D, spawns: { regions: {} }, events: sink, loadout: () => ({ willLevel: 1, spells: { heal_life: 1 } }) });
  sim.addHero(7, { x: 0, y: 0, z: 0 });
  const h = sim.hero(7)!;
  h.health = 50;
  sim.applyInput(7, { seq: 1, moveX: 0, moveY: 0, buttons: BUTTONS.CAST | (6 << SPELL_SHIFT), cameraYaw: 0 });
  assert.equal(h.health, 65);
  // Holding the button doesn't recast.
  sim.applyInput(7, { seq: 2, moveX: 0, moveY: 0, buttons: BUTTONS.CAST | (6 << SPELL_SHIFT), cameraYaw: 0 });
  assert.equal(h.health, 65);
});

test('shops: stock and prices are server-side; platinum and phials are never for sale', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  app.economy.grant(hero.id, 5000, 'gm_grant', key(), 'gm:1');
  assert.throws(() => new Shops(app.economy, [{ id: 's', region: 'R', stock: ['OBJECT_RESURRECTION_PHIAL'] }]), /never sell/);
  assert.throws(() => new Shops(app.economy, [{ id: 's', region: 'R', stock: ['OBJECT_HERO_HELMET_PLATINUM'] }]), /never sell/);
  const shops = new Shops(app.economy, [{ id: 'smith', region: 'R', pos: { x: 0, y: 0, z: 0 }, stock: ['OBJECT_IRON_LONGSWORD'] }]);
  const price = app.economy.catalog.get('OBJECT_IRON_LONGSWORD').buy!;
  assert.throws(() => shops.buy(hero.id, 'smith', 'OBJECT_HEALTH_POTION', 1, key()), /does not sell/);
  assert.throws(() => shops.buy(hero.id, 'smith', 'OBJECT_IRON_LONGSWORD', 1, key(), { region: 'R', pos: { x: 50, y: 0, z: 0 } }), /not at that shop/);
  const r = shops.buy(hero.id, 'smith', 'OBJECT_IRON_LONGSWORD', 1, key(), { region: 'R', pos: { x: 1, y: 1, z: 0 } });
  assert.equal(app.economy.balance(hero.id), 5000 - price);
  const s = shops.sell(hero.id, 'smith', r.itemId, key());
  assert.equal(s.gold, Math.floor(price * 0.25));
  assert.equal(app.economy.audit().ok, true);
  app.close();
});

test('staff-only gear can not be minted for players, by loot or anything else', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  assert.throws(() => app.economy.mintItem(hero.id, 'OBJECT_HERO_HELMET_PLATINUM', 1, 'loot', key()), /staff-only/);
  assert.ok(app.sink.events.some((e) => e.type === 'staff_item_blocked'));
  app.economy.mintItem(hero.id, 'OBJECT_HERO_HELMET_PLATINUM', 1, 'gm', key(), 'gm:1'); // staff can
  app.close();
});

test('quest rewards: only real quests, never over the card, never twice, daily cap', () => {
  const app = testApp({ questDailyGoldCap: 800 });
  const { hero } = makeHero(app);
  const q = app.quests;
  assert.deepEqual(q.claim(hero.id, { quest: 'Q_MadeUp', gold: 10 }), { ok: false, reason: 'unknown_quest' });
  assert.deepEqual(q.claim(hero.id, { quest: 'OBJECT_QUEST_CARD_TEST_WOLVES', gold: 999_999 }), { ok: false, reason: 'over_cap' });
  assert.deepEqual(q.claim(hero.id, { quest: 'OBJECT_QUEST_CARD_TEST_WOLVES', gold: 500, renown: 101 }), { ok: false, reason: 'over_cap' });
  assert.deepEqual(q.claim(hero.id, { quest: 'OBJECT_QUEST_CARD_TEST_WOLVES', gold: 500, items: ['OBJECT_IRON_LONGSWORD'] }), { ok: false, reason: 'item_not_allowed' });
  const ok = q.claim(hero.id, { quest: 'OBJECT_QUEST_CARD_TEST_WOLVES', gold: 500, renown: 100, items: ['OBJECT_HEALTH_POTION'] });
  assert.deepEqual(ok, { ok: true, gold: 500, renown: 100, items: ['OBJECT_HEALTH_POTION'] });
  assert.equal(app.economy.balance(hero.id), 500);
  assert.equal(app.heroes.get(hero.id)!.renown, 100);
  assert.deepEqual(q.claim(hero.id, { quest: 'OBJECT_QUEST_CARD_TEST_WOLVES', gold: 500 }), { ok: false, reason: 'already_claimed' });
  // Rejected attempts are logged for the cheat dashboards.
  assert.equal(app.sink.events.filter((e) => e.type === 'quest_reward_rejected').length, 5);
  assert.equal(app.economy.audit().ok, true);
  app.close();
});

test('quest rewards: repeatable quests and the daily cap', () => {
  const app = testApp({ questDailyGoldCap: 300 });
  const { hero } = makeHero(app);
  const q = new (app.quests.constructor as typeof import('../src/economy/quests.js').QuestRewards)(
    app.db,
    app.economy,
    app.sink,
    { V_TestMinigame: { maxGold: 200, repeatMs: 60_000 } },
    [],
    {},
    { defaultMaxGold: 0, dailyGoldCap: 300 },
  );
  const t0 = 1_000_000_000_000;
  assert.equal(q.claim(hero.id, { quest: 'V_TestMinigame', gold: 200 }, t0).ok, true);
  assert.deepEqual(q.claim(hero.id, { quest: 'V_TestMinigame', gold: 200 }, t0 + 1000), { ok: false, reason: 'already_claimed' });
  assert.deepEqual(q.claim(hero.id, { quest: 'V_TestMinigame', gold: 200 }, t0 + 61_000), { ok: false, reason: 'daily_cap' });
  assert.equal(q.claim(hero.id, { quest: 'V_TestMinigame', gold: 100 }, t0 + 61_000).ok, true);
  app.close();
});
