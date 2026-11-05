import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { createApp } from '../src/app.js';
import { MemorySink } from '../src/analytics/events.js';
import { listen } from '../src/net/server.js';
import { Shops, MAX_MARKUP } from '../src/economy/shops.js';
import { WorldClock } from '../src/world/clock.js';
import { FALLBACK_TOWN, type TownData } from '../src/world/town/town-data.js';
import { shopsFromTown } from '../src/world/town/town-shops.js';
import { key, makeHero, testApp } from './helpers.js';
import { GameError } from '../src/errors.js';

test('shops from town data: one per shop building, stock from its shop def, unknown items dropped', () => {
  const app = testApp();
  try {
    const town: TownData = {
      ...FALLBACK_TOWN,
      shopTypes: { ...FALLBACK_TOWN.shopTypes, BUILDING_TEST_GENERAL_STORE: [...FALLBACK_TOWN.shopTypes.BUILDING_TEST_GENERAL_STORE!, { def: 'OBJECT_NOT_IN_CATALOG', quantity: 1, maxStock: 1, priceMultiplier: 1, restockDays: 1 }, { def: 'OBJECT_RESURRECTION_PHIAL', quantity: 1, maxStock: 1, priceMultiplier: 1, restockDays: 1 }] },
    };
    const defs = shopsFromTown(town, app.economy.catalog);
    assert.deepEqual(defs.map((d) => d.id), ['TestRegion:s1', 'TestRegion:s2']);
    assert.deepEqual(defs[0]!.stock, ['OBJECT_HEALTH_POTION', 'OBJECT_WILL_POTION']);
    assert.equal(defs[0]!.reach, 14);
    assert.ok(new Shops(app.economy, defs));
  } finally {
    app.close();
  }
});

test('shop stock: markup, limited stock, sold out, restock after the restock period, replays counted once', () => {
  const app = testApp();
  let day = 0;
  try {
    const shops = new Shops(
      app.economy,
      [
        {
          id: 'T:1',
          region: 'TestRegion',
          stock: ['OBJECT_IRON_LONGSWORD', 'OBJECT_HEALTH_POTION', 'OBJECT_WILL_POTION'],
          lines: [
            { def: 'OBJECT_IRON_LONGSWORD', maxStock: 2, priceMultiplier: 1.2, restockDays: 3 },
            { def: 'OBJECT_HEALTH_POTION', maxStock: 5, priceMultiplier: 0.2, restockDays: 0 },
            { def: 'OBJECT_WILL_POTION', maxStock: 1, priceMultiplier: 50, restockDays: 1 },
          ],
        },
      ],
      { day: () => day },
    );
    const { hero } = makeHero(app);
    app.economy.grant(hero.id, 10_000, 'gm_grant', key(), 'gm:test');
    const base = app.economy.catalog.get('OBJECT_IRON_LONGSWORD').buy!;
    assert.equal(shops.price('T:1', 'OBJECT_IRON_LONGSWORD'), Math.ceil(base * 1.2));
    assert.equal(shops.price('T:1', 'OBJECT_HEALTH_POTION'), app.economy.catalog.get('OBJECT_HEALTH_POTION').buy, 'never below base');
    assert.equal(shops.price('T:1', 'OBJECT_WILL_POTION'), app.economy.catalog.get('OBJECT_WILL_POTION').buy! * MAX_MARKUP, 'markup capped');

    const k = key();
    const before = app.economy.balance(hero.id);
    shops.buy(hero.id, 'T:1', 'OBJECT_IRON_LONGSWORD', 1, k);
    assert.equal(app.economy.balance(hero.id), before - Math.ceil(base * 1.2));
    assert.equal(shops.buy(hero.id, 'T:1', 'OBJECT_IRON_LONGSWORD', 1, k).replayed, true);
    assert.equal(shops.listing('T:1').find((l) => l.def === 'OBJECT_IRON_LONGSWORD')!.left, 1, 'replay not counted');
    shops.buy(hero.id, 'T:1', 'OBJECT_IRON_LONGSWORD', 1, key());
    assert.throws(() => shops.buy(hero.id, 'T:1', 'OBJECT_IRON_LONGSWORD', 1, key()), (e: GameError) => e.code === 'out_of_stock');
    day = 2;
    assert.equal(shops.listing('T:1')[0]!.left, 0);
    day = 3;
    assert.equal(shops.listing('T:1')[0]!.left, 2, 'restocked');
    assert.equal(shops.listing('T:1')[1]!.left, null, 'restock period 0 = never runs out');
    assert.throws(() => app.economy.vendorBuy(hero.id, 'OBJECT_IRON_LONGSWORD', 1, key(), base - 1), (e: GameError) => e.code === 'bad_price');
    assert.ok(app.economy.audit().ok);
  } finally {
    app.close();
  }
});

function connect(url: string) {
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  const waiters: [(m: any) => boolean, (m: any) => void][] = [];
  ws.on('message', (d) => {
    const m = JSON.parse(d.toString());
    inbox.push(m);
    for (const w of [...waiters]) if (w[0](m)) {
      waiters.splice(waiters.indexOf(w), 1);
      w[1](m);
    }
  });
  const open = new Promise<void>((r) => ws.on('open', () => r()));
  const next = (pred: (m: any) => boolean) =>
    new Promise<any>((resolve, reject) => {
      const hit = inbox.find(pred);
      if (hit) return resolve(hit);
      waiters.push([pred, resolve]);
      setTimeout(() => reject(new Error('timeout')), 3000);
    });
  const send = (m: unknown) => ws.send(JSON.stringify(m));
  return { ws, open, next, inbox, send };
}

test('game socket: sky on entry, villagers in ticks, and a shop visit through the ledger', async () => {
  // The test village, moved to the region graduates arrive in, with the store next to the spawn point.
  const t = FALLBACK_TOWN.regions.TestRegion!;
  const town: TownData = {
    ...FALLBACK_TOWN,
    regions: { HeroGuildComplex: { ...t, shops: t.shops.map((s) => (s.uid === 's1' ? { ...s, x: 0, y: 4, z: 0 } : s)) } },
  };
  const sink = new MemorySink();
  const clock = new WorldClock({ dayMs: 60 * 60_000, epoch: Date.now(), startFraction: 0.5 });
  const app = createApp(
    { dbPath: ':memory:', eventsPath: '', regionsPath: '/nonexistent', gameDataPath: '', spawnsPath: '', questsPath: '', contentDir: '', townDataPath: '', tickRate: 50, townFolk: true },
    { events: sink, town, clock },
  );
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  app.director.start();
  try {
    const { acct, hero } = makeHero(app);
    const c = connect(`ws://127.0.0.1:${port}/play`);
    await c.open;
    c.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: app.accounts.issueJoinTicket(acct.id, hero.id, app.cfg.realmId) });
    const sky = await c.next((m) => m.t === 'sky');
    assert.equal(sky.night, false);
    assert.ok(Math.abs(sky.day_fraction - 0.5) < 0.01);
    const tick = await c.next((m) => m.t === 'tick' && m.entities.length > 1);
    assert.ok(tick.entities.some((e: any) => e.id >= 900_000_000), 'town folk in the world state');

    c.send({ t: 'shop_open', shop: 's1' });
    const shop = await c.next((m) => m.t === 'shop');
    assert.deepEqual(shop.items.map((i: any) => i.def), ['OBJECT_HEALTH_POTION', 'OBJECT_WILL_POTION']);
    assert.equal(shop.gold, 0);
    assert.ok(shop.keeper >= 900_000_000);

    c.send({ t: 'shop_buy', shop: 's1', def: 'OBJECT_HEALTH_POTION', qty: 1, key: 'buy-0001' });
    assert.equal((await c.next((m) => m.t === 'shop_result' && m.key === 'buy-0001')).ok, false, 'no gold yet');

    app.economy.grant(hero.id, 1000, 'gm_grant', key(), 'gm:test');
    c.send({ t: 'shop_buy', shop: 's1', def: 'OBJECT_HEALTH_POTION', qty: 2, key: 'buy-0002' });
    const bought = await c.next((m) => m.t === 'shop_result' && m.key === 'buy-0002');
    assert.equal(bought.ok, true, JSON.stringify(bought));
    assert.equal(bought.gold, 1000 - bought.price * 2);
    // Resent after a dropped connection: nothing is bought twice.
    c.send({ t: 'shop_buy', shop: 's1', def: 'OBJECT_HEALTH_POTION', qty: 2, key: 'buy-0002' });
    await new Promise((r) => setTimeout(r, 100));
    assert.equal(app.economy.balance(hero.id), 1000 - bought.price * 2);

    c.send({ t: 'shop_sell', shop: 's1', item: bought.item_id, key: 'sell-0001' });
    const sold = await c.next((m) => m.t === 'shop_result' && m.key === 'sell-0001');
    assert.equal(sold.ok, true, JSON.stringify(sold));
    assert.ok(sold.earned > 0 && sold.earned < bought.price * 2, 'selling back always loses gold');

    c.send({ t: 'shop_open', shop: 's2' });
    assert.equal((await c.next((m) => m.t === 'shop_result' && m.shop === 's2')).error, 'too_far');

    // Drawing steel in the shop sends the keeper under the counter.
    c.send({ t: 'input', seq: 1, move_x: 0, move_y: 0, buttons: 1, camera_yaw: 0 });
    await c.next((m) => m.t === 'tick' && m.last_input_seq === 1);
    await new Promise((r) => setTimeout(r, 100));
    c.send({ t: 'shop_buy', shop: 's1', def: 'OBJECT_HEALTH_POTION', qty: 1, key: 'buy-0003' });
    assert.equal((await c.next((m) => m.t === 'shop_result' && m.key === 'buy-0003')).error, 'shop_closed');
    assert.ok(sink.events.some((e) => e.type === 'shop_buy'));
    assert.ok(app.economy.audit().ok);
    c.ws.close();
  } finally {
    server.close();
    app.close();
  }
});
