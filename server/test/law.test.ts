import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero, key } from './helpers.js';
import { Law, type LawHost, type LawMessage } from '../src/law/law.js';
import { DEFAULT_RULES, mergeRules, type LawRules } from '../src/law/rules.js';
import { GameError } from '../src/errors.js';
import { ItemCatalog, STARTER_ITEMS } from '../src/economy/catalog.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;
const MIN = 60_000;

/** A Law with a hand-driven clock and dice, and a fake world that records moves. */
function setup(over: Partial<LawRules> | object = {}, opts: { roll?: number } = {}) {
  const app = testApp();
  app.economy.policies.length = 0; // drop the app's own Law; this test drives its own
  let now = Date.UTC(2026, 10, 3, 12);
  let roll = opts.roll ?? 0.99;
  const where = new Map<number, string>();
  const moves: { heroId: number; region: string; private: boolean }[] = [];
  const sent: { heroId: number; msg: LawMessage }[] = [];
  const host: LawHost = {
    regionOf: (id) => where.get(id),
    relocate: (id, region, o) => {
      moves.push({ heroId: id, region, private: o.private });
      where.set(id, region);
    },
    notify: (heroId, msg) => sent.push({ heroId, msg }),
    hasRegion: () => true,
  };
  const law = new Law(app.db, app.economy, app.sink, mergeRules(over), host, () => now, () => roll);
  return {
    app,
    law,
    where,
    moves,
    sent,
    advance: (ms: number) => (now += ms),
    /** Play online for `ms` (advances both clocks). */
    play: (ids: number[], ms: number) => {
      for (let t = 0; t < ms; t += 1000) {
        now += 1000;
        law.tick(1000, ids);
      }
    },
    setRoll: (r: number) => (roll = r),
    hero: (gold = 0) => {
      const { hero } = makeHero(app);
      if (gold) app.economy.grant(hero.id, gold, 'quest_reward', key());
      where.set(hero.id, 'BowerstoneSlums');
      return hero.id;
    },
  };
}

test('fines come from the crime table, repeats cost 1.5x, and paying sinks the gold', () => {
  const t = setup();
  const h = t.hero(5000);
  const a = t.law.record(h, 'theft', { town: 'bowerstone', value: 300 });
  assert.equal(a.fine, 600); // 2 x value
  assert.equal(t.law.record(h, 'theft', { town: 'bowerstone', value: 300 }).fine, 900);
  assert.equal(t.law.record(h, 'theft', { town: 'bowerstone', value: 10 }).fine, 225); // min 100, x1.5 x1.5
  t.advance(25 * 3600_000);
  assert.equal(t.law.record(h, 'theft', { town: 'bowerstone', value: 300 }).fine, 600, 'the repeat window has passed');
  assert.equal(t.law.quoteFine(h), 2325);
  const r = t.law.payFine(h, key());
  assert.equal(r.paid, 2325);
  assert.equal(t.app.economy.balance(h), 5000 - 2325);
  assert.equal(t.law.wantedLevel(h), 0);
  const audit = t.app.economy.audit();
  assert.ok(audit.ok);
  assert.equal(audit.supply.sunk, 2325);
});

test('wanted level follows the worst crime and the unpaid total', () => {
  const t = setup();
  const h = t.hero();
  assert.equal(t.law.wantedLevel(h), 0);
  t.law.record(h, 'weapon_out', { town: 'bowerstone' });
  assert.equal(t.law.wantedLevel(h), 1);
  t.law.record(h, 'assault', { town: 'bowerstone' });
  assert.equal(t.law.wantedLevel(h), 2);
  t.law.record(h, 'murder', { town: 'bowerstone' });
  assert.equal(t.law.wantedLevel(h), 3);
  t.law.record(h, 'murder_player', { town: null, victim: 'hero:9' });
  assert.equal(t.law.wantedLevel(h), 4);

  const h2 = t.hero();
  for (let i = 0; i < 3; i++) t.law.record(h2, 'theft', { town: 'oakvale', value: 1000 }); // 2000 + 3000 + 4500
  assert.equal(t.law.wantedLevel(h2), 3, 'a big enough unpaid total means arrest');
  const s = t.law.status(h2);
  assert.deepEqual(s.lines, [{ kind: 'theft', textId: 'TEXT_GUI_CRIME_THEFT', count: 3, gold: 9500 }]);
});

test('petty crimes are forgiven after an hour of online play, never by logging off', () => {
  const t = setup();
  const h = t.hero();
  t.law.record(h, 'weapon_out', { town: 'bowerstone' });
  t.law.record(h, 'theft', { town: 'bowerstone', value: 50 });
  t.advance(5 * 3600_000); // offline
  assert.equal(t.law.openCrimes(h).length, 2);
  assert.equal(t.law.status(h).forgivenInMinutes, 60);
  t.play([h], 61 * MIN);
  const open = t.law.openCrimes(h);
  assert.deepEqual(open.map((c) => c.kind), ['theft'], 'theft is never forgiven');
});

test('bribes always cost more than the fine, double per bribe in a day, and never cover murder', () => {
  const t = setup();
  const h = t.hero(20_000);
  t.law.record(h, 'weapon_out', { town: 'bowerstone' }); // fine 20
  assert.equal(t.law.quoteBribe(h, 'bowerstone'), 200, 'at least 200');
  t.law.bribe(h, 'bowerstone', key());
  assert.equal(t.law.wantedLevel(h), 0);
  assert.ok(t.law.isBlind(h, 'bowerstone'));
  t.law.record(h, 'theft', { town: 'bowerstone', value: 500 }); // fine 1000
  assert.equal(t.law.quoteBribe(h, 'bowerstone'), 3000, '1.5 x 1000, doubled for the second bribe today');
  assert.ok(t.law.quoteBribe(h, 'bowerstone')! > t.law.quoteFine(h));
  assert.equal(t.law.quoteBribe(h, 'oakvale'), null, 'a bribe only clears that town');
  t.law.record(h, 'murder', { town: 'bowerstone' });
  assert.equal(t.law.quoteBribe(h, 'bowerstone'), null);
  assert.throws(() => t.law.bribe(h, 'bowerstone', key()), code('no_bribe'));
  t.advance(24 * 3600_000);
  t.law.payFine(h, key());
  t.law.record(h, 'weapon_out', { town: 'bowerstone' });
  assert.equal(t.law.quoteBribe(h, 'bowerstone'), 200, 'the doubling resets the next day');
});

test('a level-2 fine throws the hero out of town, counted in online time', () => {
  const t = setup();
  const h = t.hero(1000);
  t.law.record(h, 'assault', { town: 'bowerstone', victim: 'CREATURE_BS_VILLAGER' });
  const r = t.law.payFine(h, key(), 'bowerstone');
  assert.deepEqual(r.kicked, { town: 'bowerstone', minutes: 10 });
  assert.deepEqual(t.moves.at(-1), { heroId: h, region: 'LookoutPoint', private: false });
  assert.equal(t.law.checkEnter(h, 'BowerstonePosh', 'exit')?.code, 'kicked_out');
  assert.equal(t.law.checkEnter(h, 'OakValeWest_v2', 'exit'), null);
  t.advance(3600_000); // logged off for an hour: still out
  assert.equal(t.law.checkEnter(h, 'BowerstonePosh', 'exit')?.code, 'kicked_out');
  t.play([h], 10 * MIN);
  assert.equal(t.law.checkEnter(h, 'BowerstonePosh', 'exit'), null);
});

test('wanted heroes cannot use shops or Cullis Gates', () => {
  const t = setup();
  const h = t.hero(5000);
  t.app.economy.vendorBuy(h, 'Apple', 1, key());
  t.law.record(h, 'assault', { town: 'bowerstone' });
  assert.throws(() => t.app.economy.vendorBuy(h, 'Apple', 1, key()), code('wanted'));
  assert.equal(t.law.checkEnter(h, 'Witchwood', 'cullis_gate')?.code, 'wanted');
  t.law.payFine(h, key());
  t.app.economy.vendorBuy(h, 'Apple', 1, key());
});

test('NPC murder means jail; time only counts online; paying out costs the fines plus 25%', () => {
  const t = setup();
  const h = t.hero(10_000);
  t.law.record(h, 'murder', { town: 'bowerstone' });
  const s = t.law.sentence(h, 'caught');
  assert.equal(s.prison, 'jail');
  assert.equal(s.total_ms, 10 * MIN); // 2,000 gold of fines at 5 min per 1,000
  assert.deepEqual(t.moves.at(-1), { heroId: h, region: 'BowerstoneJail', private: true });
  assert.equal(t.law.wantedLevel(h), 0, 'the sentence settles the record');
  assert.equal(t.law.checkEnter(h, 'BowerstonePosh', 'exit')?.code, 'in_prison');
  assert.deepEqual(t.law.placement(h), { region: 'BowerstoneJail', private: true });
  t.advance(3600_000);
  assert.ok(t.law.openSentence(h), 'logged off time does not count');
  t.play([h], 4 * MIN);
  const r = t.law.payOut(h, key());
  assert.equal(r.paid, 2500);
  assert.equal(t.law.openSentence(h), undefined);
  assert.equal(t.moves.at(-1)!.region, 'BowerstonePosh');
});

test('serving the full sentence releases the hero', () => {
  const t = setup();
  const h = t.hero();
  t.law.record(h, 'theft', { town: 'bowerstone', value: 100 });
  const s = t.law.sentence(h, 'unpaid_fine');
  assert.equal(s.total_ms, 2 * MIN, 'minimum jail time');
  t.play([h], 2 * MIN + 1000);
  assert.equal(t.law.openSentence(h), undefined);
  assert.ok(t.app.sink.events.some((e) => e.type === 'released' && e.heroId === h && e.how === 'served'));
});

test('the fourth jail trip in a week goes to Bargate; murdering a hero always does, doubling on repeats', () => {
  const t = setup();
  const h = t.hero();
  for (let i = 0; i < 3; i++) {
    t.law.record(h, 'theft', { town: 'bowerstone', value: 100 });
    assert.equal(t.law.sentence(h, 'caught').prison, 'jail');
    t.play([h], 3 * MIN);
  }
  t.law.record(h, 'theft', { town: 'bowerstone', value: 100 });
  assert.equal(t.law.sentence(h, 'caught').prison, 'bargate');

  const k = t.hero(50_000);
  t.law.record(k, 'murder_player', { town: null, victim: 'hero:1' });
  assert.equal(t.law.quoteFine(k), 0, "a murdered hero can't be paid off");
  assert.equal(t.law.quoteBribe(k, 'bowerstone'), null);
  const s1 = t.law.sentence(k, 'caught');
  assert.equal(s1.prison, 'bargate');
  assert.equal(s1.total_ms, 20 * MIN);
  assert.ok(['PrisonCells_1', 'PrisonCells_2', 'PrisonCells_3'].includes(t.moves.at(-1)!.region));
  assert.equal(t.law.checkEnter(k, 'PrisonCourtyard', 'exit'), null, 'prisoners may walk the grounds');
  assert.equal(t.law.checkEnter(k, 'PrisonPath', 'exit')?.code, 'in_prison');
  assert.throws(() => t.law.payOut(k, key()), code('no_payout'));

  // The warden halves what's left, once, and only in his office.
  t.play([k], 2 * MIN);
  assert.throws(() => t.law.warden(k, key()), code('not_here'));
  t.where.set(k, 'PrisonOffice');
  const w = t.law.warden(k, key());
  assert.equal(w.leftMs, 9 * MIN);
  assert.equal(w.paid, 9 * 50);
  assert.equal(t.law.quoteWarden(k), null);
  t.play([k], 9 * MIN + 1000);
  assert.equal(t.law.openSentence(k), undefined);
  assert.equal(t.moves.at(-1)!.region, 'PrisonPath');

  t.law.record(k, 'murder_player', { town: null, victim: 'hero:2' });
  assert.equal(t.law.sentence(k, 'caught').total_ms, 40 * MIN);
});

test('the Bargate escape stays off until it is switched on', () => {
  const off = setup();
  const h = off.hero();
  off.law.record(h, 'murder_player', { town: null });
  off.law.sentence(h, 'caught');
  assert.throws(() => off.law.escape(h, true), code('disabled'));

  const on = setup({ open: { ...DEFAULT_RULES.open, bargateEscape: true } });
  const a = on.hero();
  on.law.record(a, 'murder_player', { town: null });
  on.law.sentence(a, 'caught');
  assert.equal(on.law.escape(a, true).leftMs, 15 * MIN);
  assert.throws(() => on.law.escape(a, true), code('no_escape'), 'one try per sentence');
  const b = on.hero();
  on.law.record(b, 'murder_player', { town: null });
  on.law.sentence(b, 'caught');
  assert.equal(on.law.escape(b, false).leftMs, 40 * MIN);
});

const stealReq = (over: object = {}) => ({
  shop: 'bs_general_store',
  defId: 'IronLongsword',
  town: 'bowerstone',
  npcOwned: true,
  guardInView: false,
  isDay: false,
  stealthPieces: 0,
  clientSpotted: false,
  ...over,
});

test('an unseen steal gives a marked item that only the fence will buy, at a quarter of its value', () => {
  const t = setup({}, { roll: 0.99 });
  const h = t.hero(100);
  const other = t.hero();
  const r = t.law.steal(h, stealReq(), key());
  assert.equal(r.spotted, false);
  assert.ok(r.itemId);
  assert.ok(t.law.isMarked(r.itemId!));
  assert.throws(() => t.app.economy.vendorSell(h, r.itemId!, key()), code('stolen'));
  assert.throws(() => t.app.economy.executeTrade({ heroId: h, gold: 0, itemIds: [r.itemId!] }, { heroId: other, gold: 0, itemIds: [] }, key()), code('stolen'));
  const f = t.law.fence(h, r.itemId!, key());
  assert.equal(f.gold, 200); // 800 x 0.25
  assert.equal(t.app.economy.balance(h), 300);
  assert.ok(t.app.economy.audit().ok);
});

test('marks wear off after 72 hours', () => {
  const t = setup();
  const h = t.hero();
  const { itemId } = t.law.steal(h, stealReq({ defId: 'HealthPotion' }), key());
  t.advance(73 * 3600_000);
  assert.equal(t.law.isMarked(itemId!), false);
  assert.throws(() => t.law.fence(h, itemId!, key()), code('not_stolen'));
  t.app.economy.vendorSell(h, itemId!, key());
});

test('a spotted steal is a theft on the record and the item stays in the shop', () => {
  const t = setup({}, { roll: 0 });
  const h = t.hero();
  const r = t.law.steal(h, stealReq(), key());
  assert.equal(r.spotted, true);
  assert.equal(r.itemId, null);
  assert.equal(r.crime!.kind, 'theft');
  assert.equal(r.crime!.fine, 1600);
  assert.equal(t.app.economy.inventory(h).length, 0);
});

test('steals have a cooldown, a per-shop stock and a daily cap; player property is off limits', () => {
  const t = setup({}, { roll: 0.99 });
  const h = t.hero();
  t.law.steal(h, stealReq(), key());
  assert.throws(() => t.law.steal(h, stealReq({ defId: 'Apple' }), key()), code('too_fast'));
  t.advance(11_000);
  assert.throws(() => t.law.steal(h, stealReq(), key()), code('no_stock'));
  t.law.steal(h, stealReq({ shop: 'bs_weapon_shop' }), key()); // 1,600 stolen today
  t.advance(11_000);
  assert.throws(() => t.law.steal(h, stealReq({ shop: 'oak_shop' }), key()), code('steal_cap'));
  t.law.steal(h, stealReq({ shop: 'oak_shop', defId: 'HealthPotion' }), key());
  assert.throws(() => t.law.steal(h, stealReq({ npcOwned: false }), key()), code('not_stealable'));
  assert.throws(() => t.law.steal(h, stealReq({ defId: 'ResurrectionPhial' }), key()), code('not_stealable'));
  t.advance(24 * 3600_000);
  t.law.steal(h, stealReq(), key()); // a new day: stock and cap refill
});

test('the spotted chance rises with guards, daylight and repeats, falls with stealth, and the client can only raise it', () => {
  const t = setup();
  const h = t.hero();
  const chance = (over: object) => {
    t.advance(11_000);
    const r = t.law.steal(h, stealReq({ shop: `s${Math.random()}`, defId: 'Apple', ...over }), key());
    return r.chance;
  };
  assert.equal(chance({}), 0.25);
  assert.equal(chance({}), 0.3, 'a repeat steal within the hour');
  assert.equal(chance({ stealthPieces: 5 }), 0.2);
  assert.equal(chance({ isDay: true, guardInView: true }), 0.95);
  assert.ok(chance({ clientSpotted: true }) > 0.5);
  assert.ok(chance({ stealthPieces: 40 }) >= 0.05, 'never below the floor');
});

test('stealing loses gold on average: fines outweigh what the fence pays', () => {
  const rules = DEFAULT_RULES;
  let seed = 7;
  const rng = () => ((seed = (seed * 16807) % 2147483647) / 2147483647);
  const t = setup();
  const law = new Law(t.app.db, t.app.economy, t.app.sink, rules, null, (() => {
    let now = Date.UTC(2026, 10, 3);
    return () => (now += 3 * 3600_000); // each call moves on, so caps and cooldowns never bite
  })(), rng);
  const catalog = new ItemCatalog(STARTER_ITEMS);
  let net = 0;
  for (let i = 0; i < 300; i++) {
    const h = t.hero();
    const r = law.steal(h, stealReq({ shop: `shop${i}` }), key());
    if (r.spotted) net -= r.crime!.fine;
    else net += Math.floor(catalog.get('IronLongsword').buy! * rules.stealing.fenceRatio);
  }
  assert.ok(net < 0, `average take per steal ${net / 300}`);
});

test('pickpocketing other heroes stays off, and the rule set is fully overridable', () => {
  assert.equal(DEFAULT_RULES.open.pickpocketPlayers, false);
  assert.equal(DEFAULT_RULES.open.playerBounty, false);
  const r = mergeRules({ crimes: { theft: { valueFactor: 3 } }, stealing: { fenceRatio: 0.2 } });
  assert.equal(r.crimes.theft.valueFactor, 3);
  assert.equal(r.crimes.theft.minFine, 100, 'untouched fields keep their defaults');
  assert.equal(r.stealing.fenceRatio, 0.2);
  assert.equal(r.stealing.dailyValueCap, 2000);
});

test('an old payment key never pays a new fine, bribe or jail payout for free', () => {
  const t = setup();
  const h = t.hero(20_000);
  const k = key();
  t.law.record(h, 'theft', { town: 'bowerstone', value: 300 });
  assert.equal(t.law.payFine(h, k).paid, 600);
  const gold = t.app.economy.balance(h);

  t.law.record(h, 'theft', { town: 'bowerstone', value: 300 });
  assert.throws(() => t.law.payFine(h, k), code('key_reused'));
  assert.ok(t.law.wantedLevel(h) > 0, 'the new crime stays open');
  assert.throws(() => t.law.bribe(h, 'bowerstone', k), code('key_reused'));
  assert.ok(t.law.wantedLevel(h) > 0, 'the bribe did not settle it');

  t.law.record(h, 'murder', { town: 'bowerstone' });
  t.law.sentence(h, 'caught');
  assert.throws(() => t.law.payOut(h, k), code('key_reused'));
  assert.ok(t.law.openSentence(h), 'still in jail');
  assert.equal(t.app.economy.balance(h), gold, 'no gold moved and nothing was granted');
});
