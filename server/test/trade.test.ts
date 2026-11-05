import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero, key } from './helpers.js';
import { GameError } from '../src/errors.js';
import { SETTLE_MS, TRADE_REACH, TradeSessions, type TradeHost } from '../src/economy/trade.js';
import { decode, ProtocolError } from '../src/net/protocol.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;

/** A world of two (or more) heroes standing where the test puts them, with a hand-turned clock. */
function world(n = 2) {
  const app = testApp();
  const heroes = Array.from({ length: n }, () => makeHero(app).hero.id);
  const pos = new Map<number, { channel: string; x: number; y: number; z: number }>(heroes.map((h, i) => [h, { channel: 'c1', x: i, y: 0, z: 0 }]));
  const inbox = new Map<number, Record<string, unknown>[]>(heroes.map((h) => [h, []]));
  let now = 1_000_000;
  const host: TradeHost = {
    send: (h, m) => inbox.get(h)!.push(m),
    where: (h) => pos.get(h),
    name: (h) => `H${h}`,
  };
  const trades = new TradeSessions(app.economy, host, () => now);
  const last = (h: number, t: string) => [...inbox.get(h)!].reverse().find((m) => m.t === t) as Record<string, any> | undefined;
  return {
    app,
    trades,
    heroes,
    pos,
    inbox,
    last,
    wait: (ms = SETTLE_MS) => (now += ms),
  };
}

test('invite, offer, accept: the swap is atomic and both sides hear about it', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  w.app.economy.grant(a, 1000, 'quest_reward', key());
  const sword = w.app.economy.mintItem(b, 'IronLongsword', 1, 'loot', key()).itemId;

  w.trades.request(a, b);
  assert.equal(w.last(b, 'trade_invite')!.from, a);
  w.trades.answer(b, a, true);
  const s0 = w.last(b, 'trade_state')!;
  assert.equal(s0.with, a);
  assert.deepEqual(s0.stock, [{ item: sword, def: 'IronLongsword', qty: 1 }]);

  w.trades.setOffer(a, 600, []);
  const v = w.trades.setOffer(b, 0, [sword]);
  assert.throws(() => w.trades.accept(a, v), code('too_soon'), 'no accept right after a change');
  w.wait();
  assert.equal(w.trades.accept(a, v).done, false);
  assert.equal(w.last(b, 'trade_state')!.accepted.them, true);

  // B swaps the offer after A accepted: A's tick is cleared and the old version is refused.
  const v2 = w.trades.setOffer(b, 0, []);
  assert.equal(w.last(a, 'trade_state')!.accepted.me, false);
  w.wait();
  assert.throws(() => w.trades.accept(a, v), code('offer_changed'));
  assert.throws(() => w.trades.accept(b, v), code('offer_changed'));
  const v3 = w.trades.setOffer(b, 0, [sword]);
  assert.ok(v3 > v2);
  w.wait();
  assert.equal(w.trades.accept(a, v3).done, false);
  assert.equal(w.trades.accept(b, v3).done, true);

  assert.equal(w.app.economy.balance(a), 400);
  assert.equal(w.app.economy.balance(b), 600);
  assert.equal(w.app.economy.item(sword)!.owner_hero, a);
  assert.equal(w.last(a, 'trade_done')!.gold, 400);
  assert.equal(w.last(b, 'trade_done')!.gold, 600);
  assert.equal(w.trades.isTrading(a), false);
  assert.ok(w.app.economy.audit().ok);
});

test('part of a stack is split off for the receiver; the rest stays', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  const apples = w.app.economy.mintItem(a, 'Apple', 30, 'loot', key()).itemId;
  w.trades.open(a, b);
  const v = w.trades.setOffer(a, 0, [{ id: apples, qty: 12 }]);
  assert.deepEqual(w.last(b, 'trade_state')!.theirs.items, [{ item: apples, def: 'Apple', qty: 12 }]);
  assert.throws(() => w.trades.setOffer(a, 0, [{ id: apples, qty: 31 }]), code('bad_quantity'));
  w.wait();
  w.trades.accept(a, v);
  w.trades.accept(b, v);
  assert.equal(w.app.economy.item(apples)!.quantity, 18);
  assert.equal(w.app.economy.item(apples)!.owner_hero, a);
  const got = w.app.economy.inventory(b);
  assert.equal(got.length, 1);
  assert.equal(got[0]!.def_id, 'Apple');
  assert.equal(got[0]!.quantity, 12);
  assert.notEqual(got[0]!.id, apples);
  assert.ok(w.app.economy.audit().ok);
});

test('a whole stack offered without a quantity cannot shrink before the swap', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  w.app.economy.grant(b, 300, 'quest_reward', key());
  const apples = w.app.economy.mintItem(a, 'Apple', 20, 'loot', key()).itemId;
  w.trades.open(a, b);
  w.trades.setOffer(b, 300, []);
  const v = w.trades.setOffer(a, 0, [apples]);
  assert.deepEqual(w.last(b, 'trade_state')!.theirs.items, [{ item: apples, def: 'Apple', qty: 20 }]);
  w.wait();
  w.trades.accept(b, v);
  // A eats 19 of the 20 between B's accept and their own.
  for (let i = 0; i < 19; i++) w.app.economy.consumeOne(a, 'Apple', key());
  assert.throws(() => w.trades.accept(a, v), code('bad_quantity'));
  assert.equal(w.app.economy.balance(b), 300);
  assert.equal(w.app.economy.item(apples)!.owner_hero, a);
  assert.ok(w.app.economy.audit().ok);
});

test('an item sold, moved or spent after it was offered fails the swap; nothing moves', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  w.app.economy.grant(a, 500, 'quest_reward', key());
  const sword = w.app.economy.mintItem(b, 'IronLongsword', 1, 'loot', key()).itemId;
  w.trades.open(a, b);
  w.trades.setOffer(a, 500, []);
  const v = w.trades.setOffer(b, 0, [sword]);
  w.wait();
  w.trades.accept(a, v);
  // B equips the sword between the offer and the final accept.
  w.app.economy.moveItem(b, sword, 'equipped', key());
  assert.throws(() => w.trades.accept(b, v), code('wrong_location'), "B's own stale offer is caught at accept");
  w.app.economy.moveItem(b, sword, 'inventory', key());
  w.trades.unaccept(a);
  w.trades.accept(b, v); // B ticks first this time
  // A spends the gold before ticking: the swap fails inside the ledger.
  w.app.economy.spend(a, 100, 'repair', key());
  assert.throws(() => w.trades.accept(a, v), code('insufficient_gold'));
  // Same race but past the per-side check: A's gold goes after A ticked and before B's tick.
  w.app.economy.grant(a, 100, 'quest_reward', key());
  const v2 = w.trades.setOffer(a, 500, []);
  w.wait();
  w.trades.accept(a, v2);
  w.app.economy.spend(a, 100, 'repair', key());
  const r = w.trades.accept(b, v2);
  assert.deepEqual(r, { done: false, error: 'insufficient_gold' });
  assert.equal(w.last(a, 'trade_error')!.error, 'insufficient_gold');
  const s = w.last(b, 'trade_state')!;
  assert.deepEqual(s.accepted, { me: false, them: false }, 'a failed swap clears both ticks');
  assert.ok(s.version > v2, 'and bumps the version');
  assert.equal(w.app.economy.item(sword)!.owner_hero, b);
  assert.equal(w.app.economy.balance(a), 400);
  assert.equal(w.app.economy.balance(b), 0);
  assert.ok(w.trades.isTrading(a), 'the window stays open');
  assert.ok(w.app.economy.audit().ok);
});

test('a replayed accept cannot pay twice', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  w.app.economy.grant(a, 300, 'quest_reward', key());
  w.trades.open(a, b);
  const v = w.trades.setOffer(a, 300, []);
  w.wait();
  w.trades.accept(a, v);
  w.trades.accept(b, v);
  assert.throws(() => w.trades.accept(b, v), code('no_trade'));
  assert.throws(() => w.trades.accept(a, v), code('no_trade'));
  assert.equal(w.app.economy.balance(b), 300);
  assert.equal(w.app.economy.balance(a), 0);
});

test('staff gear, soulbound and stolen items never go in the window', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  const phial = w.app.economy.mintItem(a, 'ResurrectionPhial', 1, 'quest_reward', key()).itemId;
  const seal = w.app.economy.mintItem(a, 'GuildSeal', 1, 'quest_reward', key()).itemId;
  const apple = w.app.economy.mintItem(a, 'Apple', 1, 'loot', key()).itemId;
  w.app.economy.catalog.add({ id: 'TEST_STAFF_PLATE', maxStack: 1, buy: null, sell: null, tradeable: false, staffOnly: true });
  const plate = w.app.economy.mintItem(a, 'TEST_STAFF_PLATE', 1, 'gm', key(), 'gm:test').itemId;
  const stolen = new Set([apple]);
  w.app.economy.policies.push({ item: (id, use) => (use === 'trade' && stolen.has(id) ? new GameError('stolen', 'Stolen goods cannot be traded') : null) });

  w.trades.open(a, b);
  assert.deepEqual(w.last(a, 'trade_state')!.stock, [], 'nothing on offer in the picker');
  assert.throws(() => w.trades.setOffer(a, 0, [phial]), code('untradeable'));
  assert.throws(() => w.trades.setOffer(a, 0, [seal]), code('untradeable'));
  assert.throws(() => w.trades.setOffer(a, 0, [plate]), code('untradeable'));
  assert.throws(() => w.trades.setOffer(a, 0, [apple]), code('stolen'));
  assert.ok(w.app.sink.events.some((e) => e.type === 'staff_item_blocked' && e.defId === 'TEST_STAFF_PLATE'));
  // Straight to the ledger as well.
  assert.throws(() => w.app.economy.executeTrade({ heroId: a, gold: 0, itemIds: [plate] }, { heroId: b, gold: 0, itemIds: [] }, key()), code('untradeable'));
  assert.equal(w.app.economy.item(plate)!.owner_hero, a);
});

test('B cannot offer an item that belongs to A, or offer one twice', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  const sword = w.app.economy.mintItem(a, 'IronLongsword', 1, 'loot', key()).itemId;
  w.trades.open(a, b);
  assert.throws(() => w.trades.setOffer(b, 0, [sword]), code('not_owner'));
  assert.throws(() => w.trades.setOffer(a, 0, [sword, sword]), code('bad_trade'));
  assert.throws(() => w.trades.setOffer(a, 1, []), code('insufficient_gold'));
  for (const bad of [-1, 1.5, NaN, 2 ** 60]) assert.throws(() => w.trades.setOffer(a, bad, []), code('bad_amount'));
  assert.throws(() => w.trades.setOffer(a, 0, Array.from({ length: 17 }, () => sword)), code('bad_trade'));
});

test('distance, channel, one window at a time, and invites that run out', () => {
  const w = world(3);
  const [a, b, c] = w.heroes as [number, number, number];
  w.pos.get(b)!.x = TRADE_REACH + 5;
  assert.throws(() => w.trades.request(a, b), code('too_far'));
  w.pos.get(b)!.x = 1;
  w.pos.get(c)!.channel = 'c2';
  assert.throws(() => w.trades.request(a, c), code('too_far'));
  assert.throws(() => w.trades.request(a, a), code('bad_trade'));
  w.pos.get(c)!.channel = 'c1';

  // Invites expire.
  w.trades.request(a, b);
  w.wait(31_000);
  assert.throws(() => w.trades.answer(b, a, true), code('no_invite'));
  // Declined.
  w.trades.request(a, b);
  w.trades.answer(b, a, false);
  assert.equal(w.last(a, 'trade_declined')!.by, b);
  // Asking each other opens at once.
  w.trades.request(a, b);
  w.trades.request(b, a);
  assert.ok(w.trades.isTrading(a) && w.trades.isTrading(b));
  assert.throws(() => w.trades.request(c, a), code('busy'));

  // Walking away closes the window at the next check.
  w.pos.get(b)!.x = 50;
  w.trades.tick(1000);
  assert.equal(w.last(a, 'trade_closed')!.reason, 'too_far');
  assert.equal(w.trades.isTrading(a), false);

  // Logging out closes it too.
  w.pos.get(b)!.x = 1;
  w.trades.open(a, b);
  w.trades.left(b);
  assert.equal(w.last(a, 'trade_closed')!.reason, 'left');
  // And 5 quiet minutes.
  w.trades.open(a, c);
  w.wait(5 * 60_000 + 1);
  w.trades.sweep();
  assert.equal(w.last(c, 'trade_closed')!.reason, 'expired');
});

test('heroes in prison or wanted cannot trade', () => {
  const w = world();
  const [a, b] = w.heroes as [number, number];
  const jailed = new Set<number>();
  w.app.economy.policies.push({ service: (h, s) => (s === 'trade' && jailed.has(h) ? new GameError('in_prison', 'You are serving a sentence', 403) : null) });
  jailed.add(b);
  assert.throws(() => w.trades.request(a, b), code('in_prison'));
  jailed.delete(b);
  w.app.economy.grant(a, 50, 'quest_reward', key());
  w.trades.open(a, b);
  const v = w.trades.setOffer(a, 50, []);
  w.wait();
  w.trades.accept(a, v);
  jailed.add(b); // arrested with the window open
  assert.deepEqual(w.trades.accept(b, v), { done: false, error: 'in_prison' });
  assert.equal(w.app.economy.balance(b), 0);
  w.trades.tick(1000);
  assert.equal(w.last(a, 'trade_closed')!.reason, 'blocked');
  // The law's own check covers trading.
  assert.equal(w.app.law.serviceBlock(a, 'trade'), null);
});

test('protocol: trade messages are checked before they reach the server', () => {
  assert.deepEqual(decode(JSON.stringify({ t: 'trade_request', to: 7 })), { t: 'trade_request', to: 7 });
  assert.deepEqual(decode(JSON.stringify({ t: 'trade_offer', gold: 5, items: ['abcdef12-3456', { id: 'abcdef12-9999', qty: 3 }] })), {
    t: 'trade_offer',
    gold: 5,
    items: [{ id: 'abcdef12-3456' }, { id: 'abcdef12-9999', qty: 3 }],
  });
  for (const bad of [
    { t: 'trade_request', to: -1 },
    { t: 'trade_request', to: 'x' },
    { t: 'trade_offer', gold: -1, items: [] },
    { t: 'trade_offer', gold: 1.5, items: [] },
    { t: 'trade_offer', gold: 0, items: [{ id: 'abcdef12', qty: 0 }] },
    { t: 'trade_offer', gold: 0, items: ["x'; DROP"] },
    { t: 'trade_offer', gold: 0, items: Array.from({ length: 17 }, () => 'abcdef12') },
    { t: 'trade_accept', version: -1 },
  ])
    assert.throws(() => decode(JSON.stringify(bad)), ProtocolError, JSON.stringify(bad));
});
