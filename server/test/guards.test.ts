import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero, key } from './helpers.js';
import { Law, type LawHost } from '../src/law/law.js';
import { DEFAULT_RULES } from '../src/law/rules.js';
import { NavSim } from '../src/world/nav-sim.js';
import { field } from './nav-fixture.js';
import { profileFor } from '../src/world/ai/profiles.js';
import type { ShopDef } from '../src/economy/shops.js';

const TICK = 50;

/** A town shop at (20, 22) selling a sword and apples, as shopsFromTown builds them. */
const STORE = (region: string, uid = 'general_store'): ShopDef => ({
  id: `${region}:${uid}`,
  region,
  pos: { x: 20, y: 22, z: 0 },
  reach: 6,
  stock: ['IronLongsword', 'Apple'],
});

/** A 40 x 40 patch of Bowerstone with the law wired in and a hand-driven clock. */
function town(region = 'BowerstoneSlums', shops: ShopDef[] = [STORE(region)]) {
  const app = testApp();
  app.economy.policies.length = 0;
  let now = Date.UTC(2026, 10, 3, 12);
  const moves: { heroId: number; region: string }[] = [];
  const out: { to: number | 'all'; msg: Record<string, unknown> }[] = [];
  const host: LawHost = {
    regionOf: () => region,
    relocate: (heroId, r) => void moves.push({ heroId, region: r }),
    notify: (heroId, msg) => void out.push({ to: heroId, msg }),
    hasRegion: () => true,
  };
  const law = new Law(app.db, app.economy, app.sink, DEFAULT_RULES, host, () => now, () => 0.5);
  const sim = new NavSim(field(), undefined, 1, {
    region,
    channelId: `${region}#1`,
    law: {
      law,
      events: app.sink,
      rng: () => 0,
      shop: (id) => shops.find((s) => s.id === id),
      out: {
        send: (heroId, msg) => void out.push({ to: heroId, msg }),
        broadcast: (_c, msg) => void out.push({ to: 'all', msg }),
      },
    },
  });
  const run = (ms: number, each?: () => void) => {
    for (let t = 0; t < ms; t += TICK) {
      now += TICK;
      sim.step(TICK);
      law.tick(TICK, [...heroIds]);
      each?.();
    }
  };
  const heroIds = new Set<number>();
  const hero = (at: { x: number; y: number }, gold = 0) => {
    const { hero } = makeHero(app);
    if (gold) app.economy.grant(hero.id, gold, 'quest_reward', key());
    sim.addHero(hero.id, { ...at, z: 0 });
    heroIds.add(hero.id);
    return hero.id;
  };
  const barks = () => out.filter((o) => o.msg.t === 'law_bark').map((o) => String(o.msg.text));
  const prompts = (heroId: number) => out.filter((o) => o.to === heroId && o.msg.t === 'law_prompt');
  return { app, law, sim, run, hero, out, moves, barks, prompts, chLaw: sim.law! };
}

test('guards pick the guard profile and never flee', () => {
  assert.equal(profileFor({ defName: 'CREATURE_BS_GUARD' }).family, 'guard');
  assert.equal(profileFor({ defName: 'CREATURE_BS_GUARD_CROSSBOW' }).preferredRange, 10);
  assert.equal(profileFor({ defName: 'CREATURE_KHG_GUARD' }).fleeAt, 0);
  assert.equal(profileFor({ defName: 'CREATURE_BS_SHERIFF' }).family, 'guard');
});

test('a weapon out in town gets three warnings, then a fine the guard comes to collect', () => {
  const t = town();
  const guard = t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 10, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 20 }, 500);
  t.chLaw.setWeaponOut(h, true);
  t.run(16_000);
  const warned = t.barks();
  assert.ok(warned.some((b) => b.includes('WARNING_WEAPON_OUT_FIRST')));
  assert.ok(warned.some((b) => b.includes('WARNING_WEAPON_OUT_SECOND')));
  assert.ok(warned.some((b) => b.includes('WARNING_WEAPON_OUT_THIRD')));
  t.run(8000);
  assert.equal(t.law.openCrimes(h)[0]?.kind, 'weapon_out');
  t.run(3000);
  const p = t.prompts(h).at(-1)!;
  assert.equal(p.msg.kind, 'fine');
  assert.equal(p.msg.fine, 20);
  assert.equal(p.msg.bribe, 200);
  assert.ok(Math.hypot(guard.pos.x - 20, guard.pos.y - 20) <= DEFAULT_RULES.guards.talkRange + 0.5, 'the guard walked up');
  t.chLaw.answer(h, 'pay', key());
  assert.equal(t.app.economy.balance(h), 480);
  assert.equal(t.law.wantedLevel(h), 0);
  assert.ok(t.barks().some((b) => b.includes('RECEIVE_FINE')));
  t.run(1000);
  assert.equal(t.sim.ai.state(guard.id), 'return', 'the guard goes back to its post');
});

test('sheathing the weapon after a warning ends it', () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 10, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 20 });
  t.chLaw.setWeaponOut(h, true);
  t.run(1000);
  t.chLaw.setWeaponOut(h, false);
  t.run(1000);
  assert.ok(t.barks().some((b) => b.includes('HEED_WARNING')));
  t.run(20_000);
  assert.equal(t.law.openCrimes(h).length, 0);
});

test('a murderer is chased down and arrested', () => {
  const t = town();
  const guard = t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 10, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 22 });
  t.law.record(h, 'murder', { town: 'bowerstone' });
  t.run(6000);
  assert.ok(t.barks().some((b) => b.includes('SEEN_CRIMINAL')));
  assert.equal(guard.targetId, null, 'stood down after the arrest');
  assert.equal(t.law.openSentence(h)?.prison, 'jail');
  assert.deepEqual(t.moves.at(-1), { heroId: h, region: 'BowerstoneJail' });
  assert.ok(t.app.sink.events.some((e) => e.type === 'arrest' && e.heroId === h));
});

test('walking away from a guard asking for a fine turns it into a chase', () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 1, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 6 }, 1000);
  t.law.record(h, 'theft', { town: 'bowerstone', value: 50 });
  t.run(3000);
  assert.equal(t.prompts(h).length, 1);
  // Run off north at full speed.
  t.sim.applyInput(h, { seq: 1, moveX: 0, moveY: 1, buttons: 0, cameraYaw: 0 });
  t.run(5000);
  assert.ok(t.law.isResisting(h));
  assert.ok(t.barks().some((b) => b.includes('RUN_AWAY_WHILE_WARNING')));
  t.sim.applyInput(h, { seq: 2, moveX: 0, moveY: 0, buttons: 0, cameraYaw: 0 });
  t.run(8000);
  assert.ok(t.law.openSentence(h), 'caught');
});

test("a fine the hero can't pay means jail", () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 10, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 14 }, 10);
  t.law.record(h, 'lockpicking', { town: 'bowerstone' });
  t.run(3000);
  t.chLaw.answer(h, 'pay', key());
  assert.equal(t.law.openSentence(h)?.prison, 'jail');
  assert.ok(t.out.some((o) => o.msg.t === 'law_notice' && o.msg.text === 'TEXT_GUI_CRIME_FINE_LACK_FUNDS'));
});

test('a bribe settles it and the guards ignore petty crimes for a while', () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 10, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 14 }, 1000);
  t.law.record(h, 'weapon_out', { town: 'bowerstone' });
  t.run(3000);
  t.chLaw.answer(h, 'bribe', key());
  assert.equal(t.app.economy.balance(h), 800);
  assert.ok(t.barks().some((b) => b.includes('RECEIVE_BRIBE')));
  t.chLaw.setWeaponOut(h, true);
  t.run(30_000);
  assert.equal(t.law.openCrimes(h).length, 0, 'weapon out goes unremarked while the bribe lasts');
});

test('hitting a guard is resisting: assault on the record and every guard in sight gives chase', () => {
  const t = town();
  const g1 = t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 18, z: 0 }, { faction: 'guard', yaw: 0 });
  const g2 = t.sim.spawn('CREATURE_BS_GUARD', { x: 24, y: 12, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 20 });
  t.chLaw.onHarm(h, { id: g1.id, kind: 'creature', faction: 'guard', defName: g1.defName, maxHealth: 100 }, 10, false);
  assert.equal(t.law.openCrimes(h)[0]?.kind, 'assault');
  assert.ok(t.law.isResisting(h));
  t.run(400);
  assert.equal(g1.targetId, h);
  assert.equal(g2.targetId, h);
});

test('a villager who sees an assault tells the guards, unless the villager is killed first', () => {
  const t = town();
  // The guard faces away; the villager is watching.
  const guard = t.sim.spawn('CREATURE_BS_GUARD', { x: 5, y: 5, z: 0 }, { faction: 'guard', yaw: Math.PI });
  const v = t.sim.spawn('CREATURE_BS_VILLAGER_MALE', { x: 20, y: 25, z: 0 }, { faction: 'villager', yaw: Math.PI });
  const h = t.hero({ x: 20, y: 20 });
  t.chLaw.onHarm(h, { id: v.id, kind: 'creature', faction: 'villager', defName: v.defName, maxHealth: 100 }, 10, false);
  assert.equal(t.law.openCrimes(h).length, 0, 'nothing yet: the villager has to find a guard');
  t.run(DEFAULT_RULES.guards.villagerReportMs + 500);
  assert.equal(t.law.openCrimes(h)[0]?.kind, 'assault');
  assert.ok(guard);

  const t2 = town();
  t2.sim.spawn('CREATURE_BS_GUARD', { x: 5, y: 5, z: 0 }, { faction: 'guard', yaw: Math.PI });
  const v2 = t2.sim.spawn('CREATURE_BS_VILLAGER_MALE', { x: 20, y: 25, z: 0 }, { faction: 'villager', yaw: Math.PI });
  const h2 = t2.hero({ x: 20, y: 20 });
  t2.chLaw.onHarm(h2, { id: v2.id, kind: 'creature', faction: 'villager', defName: v2.defName, maxHealth: 100 }, 10, false);
  v2.alive = false;
  t2.run(DEFAULT_RULES.guards.villagerReportMs + 500);
  assert.equal(t2.law.openCrimes(h2).length, 0, 'no witness left');
});

test('repeated blows on one villager become aggravated assault; a kill is murder', () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 25, z: 0 }, { faction: 'guard', yaw: Math.PI });
  const v = t.sim.spawn('CREATURE_BS_VILLAGER_MALE', { x: 21, y: 20, z: 0 }, { faction: 'villager' });
  const h = t.hero({ x: 20, y: 20 });
  const hit = (d: number, killed = false) => t.chLaw.onHarm(h, { id: v.id, kind: 'creature', faction: 'villager', defName: v.defName, maxHealth: 100 }, d, killed);
  hit(20);
  hit(20);
  hit(20);
  hit(20, true);
  assert.deepEqual(t.law.openCrimes(h).map((c) => c.kind), ['assault', 'gbh', 'murder']);
});

test('heroes who agreed to PvP fight freely; hurting one who did not is a crime the server always sees', () => {
  const t = town('Greatwood_1'); // wilderness: no guards
  const a = t.hero({ x: 10, y: 10 });
  const b = t.hero({ x: 11, y: 10 });
  t.chLaw.onHarm(a, { id: b, kind: 'hero', faction: 'hero', maxHealth: 100, pvp: true }, 30, true);
  assert.equal(t.law.openCrimes(a).length, 0);
  t.chLaw.onHarm(a, { id: b, kind: 'hero', faction: 'hero', maxHealth: 100, pvp: false }, 30, true);
  assert.equal(t.law.openCrimes(a)[0]?.kind, 'murder_player');
  assert.equal(t.law.wantedLevel(a), 4);
});

test('stealing in plain view of a guard is almost always spotted and the guard comes over', () => {
  const t = town();
  const g = t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 12, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 20 }, 2000);
  const r = t.chLaw.steal(h, 'general_store', 'IronLongsword', {}, key());
  assert.equal(r.chance, 0.95);
  assert.equal(r.spotted, true); // rng 0.5 < 0.95
  assert.ok(t.barks().some((b) => b.includes('SEEN_MODERATE_CRIME')));
  t.run(3000);
  assert.equal(t.prompts(h).length, 1);
  assert.ok(Math.hypot(g.pos.x - 20, g.pos.y - 20) < 3);
});

test('a steal must name a real shop in this region, an item it stocks, and the hero must be at it', () => {
  const t = town('BowerstoneSlums', [STORE('BowerstoneSlums'), STORE('BowerstoneSouth', 'far_shop')]);
  const h = t.hero({ x: 20, y: 20 }, 2000);
  const code = (c: string) => (e: unknown) => (e as { code?: string }).code === c;
  assert.throws(() => t.chLaw.steal(h, 'made_up_shop', 'IronLongsword', {}, key()), code('no_shop'));
  assert.throws(() => t.chLaw.steal(h, 'far_shop', 'IronLongsword', {}, key()), code('no_shop'), 'a shop in another region');
  assert.throws(() => t.chLaw.steal(h, 'general_store', 'HealthPotion', {}, key()), code('not_stocked'));
  t.sim.hero(h)!.pos.y = 40;
  assert.throws(() => t.chLaw.steal(h, 'general_store', 'IronLongsword', {}, key()), code('too_far'));
  t.sim.hero(h)!.pos.y = 20;
  t.chLaw.steal(h, 'general_store', 'IronLongsword', {}, key());
  const row = t.app.db.get<{ shop: string }>('SELECT shop FROM law_steals WHERE hero_id = ?', h);
  assert.equal(row?.shop, 'BowerstoneSlums:general_store', 'stock is counted against the real shop id');
});

test('a hero felled by the watch is arrested, not killed', () => {
  const t = town();
  t.sim.spawn('CREATURE_BS_GUARD', { x: 20, y: 18, z: 0 }, { faction: 'guard', yaw: 0 });
  const h = t.hero({ x: 20, y: 20 });
  t.law.record(h, 'murder', { town: 'bowerstone' });
  t.sim.hero(h)!.health = 5;
  // Keep running so the guard has to hit instead of holding.
  t.run(3000);
  assert.ok(t.law.openSentence(h));
  assert.ok(t.sim.hero(h)!.health > 0);
});

test('in the running server: a kicked-out hero cannot walk back in, and a prisoner logs back in to the cell', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  app.economy.grant(hero.id, 1000, 'quest_reward', key());
  app.heroes.savePosition(hero.id, 'BowerstoneSlums', 0, 0, 0);
  const inbox: string[] = [];
  const link = { heroId: hero.id, name: 'h', renown: 0, lastSeq: -1, joinedAt: Date.now(), send: (f: string) => void inbox.push(f) };
  app.director.join(app.heroes.get(hero.id)!, link);
  app.law.record(hero.id, 'assault', { town: 'bowerstone' });
  app.law.payFine(hero.id, key(), 'bowerstone');
  app.director.tick(50); // the move runs after the frame
  assert.equal(app.director.channelOf(hero.id)?.region.name, 'LookoutPoint');
  assert.throws(() => app.director.transfer(hero.id, 'BowerstoneSlums', { x: 0, y: 0, z: 0 }, 'exit'), (e: unknown) => (e as { code?: string }).code === 'kicked_out');

  app.law.record(hero.id, 'murder', { town: 'bowerstone' });
  app.law.sentence(hero.id, 'caught');
  app.director.tick(50);
  const cell = app.director.channelOf(hero.id)!;
  assert.equal(cell.region.name, 'BowerstoneJail');
  assert.equal(cell.kind, 'private');
  assert.throws(() => app.director.transfer(hero.id, 'BowerstonePosh', { x: 0, y: 0, z: 0 }, 'exit'), (e: unknown) => (e as { code?: string }).code === 'in_prison');
  app.director.leave(hero.id);
  const again = app.director.join(app.heroes.get(hero.id)!, { ...link, joinedAt: Date.now() });
  assert.equal(again.region.name, 'BowerstoneJail', 'logging off does not get you out');
  assert.ok(inbox.some((f) => f.includes('"law_status"')));
  app.close();
});
