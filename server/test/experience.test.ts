import { test } from 'node:test';
import assert from 'node:assert/strict';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { Experience, type XpReport, type XpState } from '../src/heroes/experience.js';
import { FALLBACK_PROGRESSION, XpTables, pay, type Progression } from '../src/heroes/xp-tables.js';
import { decode, ProtocolError } from '../src/net/protocol.js';
import { BUTTONS, CombatSim } from '../src/world/combat-sim.js';
import { LootService } from '../src/economy/loot.js';
import { makeHero, testApp } from './helpers.js';

const T = new XpTables(FALLBACK_PROGRESSION);
const OPTS = { localDailyMax: 5_000, importMax: 20_000, echoMs: 15_000 };

/** A report of exactly the server's state: what the mod sends when nothing changed locally. */
function same(s: XpState, over: Partial<XpReport> = {}): XpReport {
  return { base: s.seq, general: s.general, strength: s.strength, skill: s.skill, will: s.will, stats: [...s.stats], abilities: [...s.abilities], ...over };
}
const empty = (over: Partial<XpReport> = {}): XpReport => ({ base: 0, general: 0, strength: 0, skill: 0, will: 0, stats: [], abilities: [], ...over });

function setup(opts = OPTS) {
  const app = testApp();
  const { hero } = makeHero(app);
  let now = Date.parse('2026-11-04T10:00:00Z');
  const xp = new Experience(app.db, app.sink, T, opts, () => now);
  const sent: Record<string, unknown>[] = [];
  xp.out = (_h, m) => sent.push(m);
  // First contact imports nothing from an empty save.
  xp.sync(hero.id, empty());
  return { app, hero, xp, sent, tick: (ms: number) => (now += ms) };
}

test('xp tables: price levels from the tables and pay own pool first, then general (Fable rule)', () => {
  assert.equal(T.statCost(0, 0, 2), 400 + 1000);
  assert.equal(T.statCost(0, 0, 8), null, 'past the last level');
  assert.equal(T.statCost(6, 0, 1), null, 'slots 6-8 are not real stats');
  assert.equal(T.abilityCost(2, 0, 1), 500);
  assert.equal(T.abilityCost(2, 1, 3), 5000 + 18000);
  assert.equal(T.abilityMax(2), 4);
  assert.deepEqual(pay({ general: 1000, strength: 300, skill: 0, will: 0 }, 'strength', 500), { general: 800, strength: 0, skill: 0, will: 0 });
  assert.equal(pay({ general: 100, strength: 300, skill: 0, will: 0 }, 'strength', 500), null);
  // Several defs naming one slot: the last one wins, as in Fable.
  const p: Progression = {
    stats: [],
    abilities: [
      { slot: 18, def: 'A', discipline: 'will', costs: [1] },
      { slot: 18, def: 'B', discipline: 'will', costs: [2] },
    ],
  };
  assert.equal(new XpTables(p).abilityBySlot.get(18)!.def, 'B');
});

test('xp: the first report imports the save once, up to the cap; later reports never import again', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const xp = new Experience(app.db, app.sink, T, OPTS);
  // Physique 2 (1400) + Fireball 1 (500) + 3000 general = 4900 worth: under the 20000 cap.
  const r = xp.sync(hero.id, empty({ general: 3000, stats: [2], abilities: [0, 0, 1] }));
  assert.equal(r.imported, true);
  assert.equal(r.state.stats[0], 2);
  assert.equal(r.state.abilities[2], 1);
  assert.equal(r.state.general, 3000);
  // A save edited to millions afterwards changes nothing: there is no second import.
  const again = xp.sync(hero.id, empty({ general: 9_000_000 }));
  assert.equal(again.imported, false);
  assert.equal(again.state.general, 3000);

  // A trainer-boosted save is cut at the cap: levels first, then pools.
  const other = makeHero(app).hero.id;
  const big = xp.sync(other, empty({ general: 1_000_000, stats: [7, 7] }));
  const worth = (T.statCost(0, 0, big.state.stats[0]!) ?? 0) + (T.statCost(1, 0, big.state.stats[1]!) ?? 0) + big.state.general;
  assert.ok(worth <= OPTS.importMax, `imported ${worth}`);
  assert.ok(big.state.stats[0]! < 7);
  assert.ok(app.sink.events.some((e) => e.type === 'xp_gain' && e.source === 'import' && e.capped));
  app.close();
});

test('xp: a purchase in Fable is priced by the server and paid from the right pools', () => {
  const { app, hero, xp } = setup();
  xp.grant(hero.id, { general: 1000, strength: 300 }, 'admin');
  const s = xp.state(hero.id);
  // Fable bought Physique 1 (400): 300 from Strength, 100 from general.
  const r = xp.sync(hero.id, same(s, { general: 900, strength: 0, stats: [1] }));
  assert.deepEqual(r.bought.map((b) => [b.id, b.level, b.cost]), [['physique', 1, 400]]);
  assert.equal(r.state.strength, 0);
  assert.equal(r.state.general, 900);
  assert.equal(r.state.stats[0], 1);
  assert.equal(r.gained.general, 0, 'what Fable charged is not mistaken for a gain or a loss');
  assert.ok(r.state.seq > s.seq);
  app.close();
});

test('xp: a purchase the hero cannot afford, or past the last level, is refused and the level stays', () => {
  const { app, hero, xp } = setup();
  xp.grant(hero.id, { general: 100 }, 'admin');
  const s = xp.state(hero.id);
  const r = xp.sync(hero.id, same(s, { stats: [3], abilities: [0, 0, 9] }));
  assert.deepEqual(
    r.refused.map((x) => [x.kind, x.slot, x.reason]),
    [
      ['stat', 0, 'cant_afford'],
      ['ability', 2, 'max_level'],
    ],
  );
  assert.equal(r.state.stats[0], 0);
  assert.equal(r.state.general, 100, 'nothing was spent');
  // A slot with no ability behind it is refused too.
  assert.equal(xp.sync(hero.id, same(r.state, { abilities: [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1] })).refused[0]!.reason, 'unknown');
  app.close();
});

test('xp: experience the local game adds is taken up to the daily cap, and only once', () => {
  const { app, hero, xp, tick } = setup();
  const s = xp.state(hero.id);
  // A quest gave 1200 general experience in Fable.
  const r = xp.sync(hero.id, same(s, { general: 1200 }));
  assert.equal(r.gained.general, 1200);
  assert.equal(r.state.general, 1200);
  // The same numbers against the new state as base: no gain.
  assert.equal(xp.sync(hero.id, same(r.state)).gained.general, 0);
  // Over the cap (5000 a day): the rest is dropped.
  const big = xp.sync(hero.id, same(r.state, { general: r.state.general + 10_000 }));
  assert.equal(big.gained.general, 5_000 - 1200);
  assert.ok(big.dropped > 0);
  // A new UTC day opens the cap again.
  tick(24 * 3600_000);
  const next = xp.sync(hero.id, same(big.state, { general: big.state.general + 500 }));
  assert.equal(next.gained.general, 500);
  // A report from an unknown base (an earlier server run) takes no gains, only purchases.
  assert.equal(xp.sync(hero.id, same(next.state, { base: 12345, general: next.state.general + 999 })).gained.general, 0);
  app.close();
});

test('xp: a report resent against the same old base pays its local gain once', () => {
  const { app, hero, xp } = setup();
  const s = xp.state(hero.id);
  assert.equal(xp.sync(hero.id, same(s, { general: 1200 })).gained.general, 1200);
  // The answer was lost or not yet written: the mod resends against the same base.
  const again = xp.sync(hero.id, same(s, { general: 1200 }));
  assert.equal(again.gained.general, 0);
  assert.equal(again.state.general, 1200);
  // More gained locally since: only the new part counts.
  const more = xp.sync(hero.id, same(s, { general: 1500, strength: 40 }));
  assert.deepEqual([more.gained.general, more.gained.strength], [300, 40]);
  assert.equal(more.state.general, 1500);
  app.close();
});

test('xp: a server kill pays the pools by how the hero fought, and its own orbs in Fable are not paid twice', () => {
  const { app, hero, xp, sent } = setup();
  xp.grantKill(hero.id, 100, { strength: 30, skill: 0, will: 10 }, 0.5, 'CREATURE_WOLF');
  const s = xp.state(hero.id);
  assert.equal(s.strength, 75);
  assert.equal(s.will, 25);
  assert.equal(s.general, 50);
  assert.equal(s.total, 150);
  assert.equal(sent.at(-1)?.t, 'xp_state', 'the new state goes to the hero');
  // The stand-in died in Fable too and dropped orbs worth about the same: those are the kill's.
  const before = xp.state(hero.id);
  const r = xp.sync(hero.id, same(before, { general: before.general + 120 }));
  assert.equal(r.gained.general, 0);
  assert.equal(r.dropped, 120);
  app.close();
});

test('xp: levels feed the combat loadout (Physique, Accuracy, Will, Health and spells)', () => {
  const { app, hero, xp } = setup();
  xp.grant(hero.id, { general: 50_000 }, 'admin');
  const s = xp.state(hero.id);
  xp.sync(hero.id, same(s, { stats: [2, 1, 0, 0, 3, 0, 0, 0, 0, 1], abilities: [0, 0, 2] }));
  const l = xp.loadout(hero.id);
  assert.deepEqual([l.strength, l.health, l.skill, l.willLevel], [2, 1, 3, 1]);
  assert.equal(l.spells?.fireball, 2);
  app.close();
});

test('xp: the combat sim pays kill experience into the right pool and refreshes stats after a purchase', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  app.experience.sync(hero.id, empty());
  const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };
  const table = { creatures: [{ uid: '1', def: 'CREATURE_BANDIT_GRUNT_LEVEL1', x: 0, y: 2, z: 0, yaw: 0, script: null }], generators: [] };
  const sim = new CombatSim(REGION, 'TestRegion#1', {
    data: D,
    spawns: { regions: { TestRegion: table } },
    events: app.sink,
    economy: app.economy,
    loot: new LootService(app.economy, D, {}, () => 0.1),
    progress: app.progress,
    experience: app.experience,
    loadout: (id) => ({ ...app.experience.loadout(id), equipped: ['OBJECT_IRON_LONGSWORD'] }),
    spawnOptions: { rng: () => 0.5 },
  });
  sim.addHero(hero.id, { x: 0, y: 0, z: 0 });
  const bandit = [...sim.spawns.creatures()][0]!;
  bandit.health = 1;
  sim.applyInput(hero.id, { seq: 1, moveX: 0, moveY: 0, buttons: BUTTONS.ATTACK, cameraYaw: 0 });
  sim.step(50);
  sim.applyInput(hero.id, { seq: 2, moveX: 0, moveY: 0, buttons: 0, cameraYaw: 0 });
  assert.equal(bandit.alive, false);
  const s = app.experience.state(hero.id);
  assert.ok(s.strength >= D.creatures.CREATURE_BANDIT_GRUNT_LEVEL1!.experience, 'a sword kill pays Strength');
  assert.equal(s.skill + s.will, 0);
  assert.ok(s.general > 0);

  const h = sim.hero(hero.id)!;
  const maxBefore = h.maxHealth;
  app.experience.grant(hero.id, { general: 10_000 }, 'admin');
  app.experience.sync(hero.id, same(app.experience.state(hero.id), { stats: [1, 2] }));
  sim.refreshHero(hero.id);
  assert.equal(h.strength, 1);
  assert.ok(h.maxHealth > maxBefore, 'Health raises max health');
  app.close();
});

test('protocol: xp_sync is checked strictly', () => {
  const ok = decode(JSON.stringify({ t: 'xp_sync', base: 5, pools: [1, 2, 3, 4], stats: [1, 2], abilities: [0, 1] }));
  assert.deepEqual(ok, { t: 'xp_sync', base: 5, pools: [1, 2, 3, 4], stats: [1, 2, 0, 0, 0, 0, 0, 0, 0, 0], abilities: [0, 1, ...new Array(18).fill(0)] });
  for (const bad of [
    { pools: [-1, 0, 0, 0], stats: [], abilities: [] },
    { pools: [1.5, 0, 0, 0], stats: [], abilities: [] },
    { pools: [0, 0, 0, 0], stats: new Array(11).fill(0), abilities: [] },
    { pools: [0, 0, 0, 0], stats: [99], abilities: [] },
    { pools: [0, 0, 0, 0], stats: [], abilities: 'x' },
  ])
    assert.throws(() => decode(JSON.stringify({ t: 'xp_sync', base: 0, ...bad })), ProtocolError);
});

test('xp: a report with nothing new is answered with a numbered state the mod accepts', () => {
  const { app, hero, xp } = setup();
  xp.grant(hero.id, { general: 500 }, 'admin');
  const s = xp.state(hero.id);
  const r = xp.sync(hero.id, same(s));
  assert.equal(r.bought.length + r.gained.general, 0);
  assert.ok(r.state.seq > 0, 'seq 0 is refused by the mod');
  assert.equal(r.state.seq, s.seq);
  assert.equal(r.state.general, 500);
  // The import answer is numbered too.
  const other = makeHero(app).hero.id;
  assert.ok(xp.sync(other, empty({ general: 10 })).state.seq > 0);
  app.close();
});
