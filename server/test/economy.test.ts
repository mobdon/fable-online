import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero, key } from './helpers.js';
import { ItemCatalog } from '../src/economy/catalog.js';
import { GameError } from '../src/errors.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;

test('gold comes only from mint, leaves only to sink, never negative', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  app.economy.grant(hero.id, 500, 'quest_reward', key());
  assert.equal(app.economy.balance(hero.id), 500);
  app.economy.spend(hero.id, 200, 'repair', key());
  assert.equal(app.economy.balance(hero.id), 300);
  assert.throws(() => app.economy.spend(hero.id, 301, 'repair', key()), code('insufficient_gold'));
  assert.equal(app.economy.balance(hero.id), 300);
  for (const bad of [0, -5, 1.5, NaN, Infinity, 2 ** 60]) assert.throws(() => app.economy.spend(hero.id, bad, 'repair', key()), code('bad_amount'));
  const a = app.economy.audit();
  assert.deepEqual(a, { ok: true, problems: [], supply: { minted: 500, sunk: 200, circulating: 300 } });
});

test('mint reasons are whitelisted and capped; GM grants need a GM', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  assert.throws(() => app.economy.grant(hero.id, 10, 'free_money' as never, key()), code('bad_reason'));
  assert.throws(() => app.economy.grant(hero.id, 5001, 'loot', key()), code('over_cap'));
  assert.throws(() => app.economy.grant(hero.id, 10, 'gm_grant', key()), code('forbidden'));
  app.economy.grant(hero.id, 10, 'gm_grant', key(), 'gm:1');
  assert.equal(app.economy.balance(hero.id), 10);
});

test('the same idempotency key never pays twice', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const k = key();
  const first = app.economy.grant(hero.id, 100, 'loot', k);
  const again = app.economy.grant(hero.id, 100, 'loot', k);
  assert.equal(again.replayed, true);
  assert.equal(again.txId, first.txId);
  assert.equal(app.economy.balance(hero.id), 100);
  assert.throws(() => app.economy.spend(hero.id, 1, 'repair', k), code('key_reused'));
  const buy = key();
  app.economy.grant(hero.id, 1000, 'quest_reward', key());
  const b1 = app.economy.vendorBuy(hero.id, 'HealthPotion', 1, buy);
  const b2 = app.economy.vendorBuy(hero.id, 'HealthPotion', 1, buy);
  assert.equal(b2.itemId, b1.itemId);
  assert.equal(app.economy.inventory(hero.id).length, 1);
});

test('catalog refuses prices that would let buy-then-sell make gold', () => {
  assert.throws(() => new ItemCatalog([{ id: 'X', maxStack: 1, buy: 100, sell: 51, tradeable: true }]));
  assert.throws(() => new ItemCatalog([{ id: 'X', maxStack: 1, buy: 100, sell: 100, tradeable: true }]));
  new ItemCatalog([{ id: 'X', maxStack: 1, buy: 100, sell: 50, tradeable: true }]);
});

test('vendor round trip always loses gold; phials cannot be bought or sold', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  app.economy.grant(hero.id, 1000, 'quest_reward', key());
  const { itemId } = app.economy.vendorBuy(hero.id, 'IronLongsword', 1, key());
  assert.equal(app.economy.balance(hero.id), 200);
  const s = app.economy.vendorSell(hero.id, itemId, key());
  assert.equal(s.gold, 200);
  assert.equal(app.economy.balance(hero.id), 400);
  assert.throws(() => app.economy.vendorSell(hero.id, itemId, key()), code('not_owner')); // already sold
  assert.throws(() => app.economy.vendorBuy(hero.id, 'ResurrectionPhial', 1, key()), code('not_for_sale'));
  const phial = app.economy.mintItem(hero.id, 'ResurrectionPhial', 1, 'quest_reward', key()).itemId;
  assert.throws(() => app.economy.vendorSell(hero.id, phial, key()), code('not_buyable'));
  assert.ok(app.economy.audit().ok);
});

test('item IDs are unique and split/merge conserve quantity', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  const { itemId } = app.economy.mintItem(hero.id, 'Apple', 30, 'loot', key());
  const split = app.economy.splitStack(hero.id, itemId, 10, key());
  assert.notEqual(split.itemId, itemId);
  assert.equal(app.economy.item(itemId)!.quantity, 20);
  assert.equal(app.economy.item(split.itemId)!.quantity, 10);
  assert.throws(() => app.economy.splitStack(hero.id, itemId, 20, key()), code('bad_quantity'));
  app.economy.mergeStacks(hero.id, split.itemId, itemId, key());
  assert.equal(app.economy.item(itemId)!.quantity, 30);
  assert.equal(app.economy.item(split.itemId)!.location, 'destroyed');
  assert.throws(() => app.economy.mergeStacks(hero.id, split.itemId, itemId, key()), code('not_owner'));
  const big = app.economy.mintItem(hero.id, 'Apple', 30, 'loot', key()).itemId;
  assert.throws(() => app.economy.mergeStacks(hero.id, big, itemId, key()), code('bad_merge')); // 60 > maxStack 50
  assert.ok(app.economy.audit().ok);
});

test('executeTrade re-checks everything inside the swap', () => {
  const app = testApp();
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  app.economy.grant(a.id, 1000, 'quest_reward', key());
  const sword = app.economy.mintItem(b.id, 'IronLongsword', 1, 'loot', key()).itemId;
  app.economy.executeTrade({ heroId: a.id, gold: 600, itemIds: [] }, { heroId: b.id, gold: 0, itemIds: [sword] }, key());
  assert.equal(app.economy.balance(a.id), 400);
  assert.equal(app.economy.balance(b.id), 600);
  assert.equal(app.economy.item(sword)!.owner_hero, a.id);

  // B now offers the sword it no longer owns: whole trade fails.
  assert.throws(() => app.economy.executeTrade({ heroId: a.id, gold: 100, itemIds: [] }, { heroId: b.id, gold: 0, itemIds: [sword] }, key()), code('not_owner'));
  assert.equal(app.economy.balance(a.id), 400);
  // Phials are soulbound.
  const phial = app.economy.mintItem(a.id, 'ResurrectionPhial', 1, 'quest_reward', key()).itemId;
  assert.throws(() => app.economy.executeTrade({ heroId: a.id, gold: 0, itemIds: [phial] }, { heroId: b.id, gold: 0, itemIds: [] }, key()), code('untradeable'));
  assert.ok(app.economy.audit().ok);
});

test('fuzz: 3,000 random actions never break the economy invariants', () => {
  const app = testApp();
  const heroes = Array.from({ length: 6 }, () => makeHero(app).hero.id);
  const pick = <T>(xs: T[]) => xs[Math.floor(Math.random() * xs.length)]!;
  const keys: string[] = [];
  let failures = 0;
  for (let i = 0; i < 3000; i++) {
    const h = pick(heroes);
    const inv = () => app.economy.inventory(h);
    const k = Math.random() < 0.1 && keys.length ? pick(keys) : key(); // 10% replays of old keys
    keys.push(k);
    try {
      switch (Math.floor(Math.random() * 9)) {
        case 0: app.economy.grant(h, Math.floor(Math.random() * 6000), 'loot', k); break;
        case 1: app.economy.spend(h, Math.floor(Math.random() * 500), 'repair', k); break;
        case 2: app.economy.vendorBuy(h, pick(['HealthPotion', 'Apple', 'IronLongsword']), 1 + Math.floor(Math.random() * 3), k); break;
        case 3: app.economy.vendorSell(h, pick(inv()).id, k); break;
        case 4: app.economy.splitStack(h, pick(inv()).id, 1 + Math.floor(Math.random() * 3), k); break;
        case 5: { const i1 = inv(); app.economy.mergeStacks(h, pick(i1).id, pick(i1).id, k); break; }
        case 6: {
          const o = pick(heroes);
          const mine = inv().slice(0, 2);
          const qty = Object.fromEntries(mine.filter(() => Math.random() < 0.5).map((x) => [x.id, 1 + Math.floor(Math.random() * x.quantity)]));
          app.economy.executeTrade({ heroId: h, gold: Math.floor(Math.random() * 300), itemIds: mine.map((x) => x.id), qty }, { heroId: o, gold: Math.floor(Math.random() * 300), itemIds: [] }, k);
          break;
        }
        case 7: app.economy.mintItem(h, 'Apple', 1 + Math.floor(Math.random() * 10), 'loot', k); break;
        case 8: app.economy.moveItem(h, pick(inv()).id, pick(['inventory', 'bank', 'equipped'] as const), k); break;
      }
    } catch (e) {
      if (!(e instanceof GameError) && !(e instanceof TypeError)) throw e; // TypeError: pick() on empty inventory
      failures++;
    }
  }
  const audit = app.economy.audit();
  assert.deepEqual(audit.problems, []);
  const circulating = heroes.reduce((s, h) => s + app.economy.balance(h), 0);
  assert.equal(circulating, audit.supply.minted - audit.supply.sunk);
  assert.ok(failures < 3000);
});
