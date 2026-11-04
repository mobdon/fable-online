import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { testApp, makeHero, wsClient, joinAs } from './helpers.js';
import { listen } from '../src/net/server.js';
import { ChatHub, type Conn } from '../src/chat/hub.js';
import { signToken } from '../src/chat/auth.js';
import type { ServerMsg } from '../src/chat/protocol.js';

const DAY = 24 * 3600_000;

test('first report imports renown once, capped', () => {
  const app = testApp({ renownImportMax: 1_000, renownDailyMax: 300 });
  try {
    const { hero } = makeHero(app);
    const r = app.renown.report(hero.id, 4_000);
    assert.deepEqual(r, { ok: true, renown: 1_000, gained: 1_000, kind: 'import', capped: true });
    assert.equal(app.heroes.get(hero.id)!.renown, 1_000);
    // The import is spent: the same number again only gets the daily allowance.
    const r2 = app.renown.report(hero.id, 4_000);
    assert.deepEqual(r2, { ok: true, renown: 1_300, gained: 300, kind: 'daily', capped: true });
    assert.equal(app.renown.report(hero.id, 4_000).ok && app.heroes.get(hero.id)!.renown, 1_300);
    const ev = app.sink.events.filter((e) => e.type === 'renown_report');
    assert.equal(ev.length, 3);
  } finally {
    app.close();
  }
});

test('an import under the cap takes the whole number', () => {
  const app = testApp({ renownImportMax: 5_000 });
  try {
    const { hero } = makeHero(app);
    assert.deepEqual(app.renown.report(hero.id, 1_234), { ok: true, renown: 1_234, gained: 1_234, kind: 'import', capped: false });
  } finally {
    app.close();
  }
});

test('reports never lower renown, and the daily cap resets the next UTC day', () => {
  const app = testApp({ renownImportMax: 500, renownDailyMax: 100 });
  try {
    const { hero } = makeHero(app);
    const t0 = Date.UTC(2026, 10, 4, 12);
    app.renown.report(hero.id, 500, t0);
    assert.equal(app.renown.report(hero.id, 10, t0).ok && app.heroes.get(hero.id)!.renown, 500);
    app.renown.report(hero.id, 650, t0);
    assert.equal(app.heroes.get(hero.id)!.renown, 600);
    app.renown.report(hero.id, 650, t0 + DAY);
    assert.equal(app.heroes.get(hero.id)!.renown, 650);
  } finally {
    app.close();
  }
});

test('renown a quest claim already paid is not counted twice', () => {
  const app = testApp({ renownImportMax: 1_000, renownDailyMax: 1_000 });
  try {
    const { hero } = makeHero(app);
    app.renown.report(hero.id, 200);
    // As if a quest claim added 100 on the server; Fable's own total now says 300.
    app.db.run('UPDATE heroes SET renown = renown + 100 WHERE id = ?', hero.id);
    const r = app.renown.report(hero.id, 300);
    assert.ok(r.ok && r.gained === 0 && r.renown === 300);
  } finally {
    app.close();
  }
});

test('bad totals are refused', () => {
  const app = testApp();
  try {
    const { hero } = makeHero(app);
    assert.deepEqual(app.renown.report(hero.id, -1), { ok: false, reason: 'bad_total' });
    assert.deepEqual(app.renown.report(hero.id, 1.5), { ok: false, reason: 'bad_total' });
    assert.deepEqual(app.renown.report(hero.id, 1e9), { ok: false, reason: 'bad_total' });
    assert.equal(app.heroes.get(hero.id)!.renown, 0);
  } finally {
    app.close();
  }
});

test('renown_report over the game socket answers and relabels the stand-in', async () => {
  const app = testApp({ tickRate: 50, renownImportMax: 2_000 });
  const server = await listen(app, 0);
  const base = `http://127.0.0.1:${(server.address() as AddressInfo).port}`;
  try {
    const { hero, ticket } = await joinAs(app, base, 'Brightwood');
    const c = await wsClient(base.replace('http', 'ws') + '/play');
    c.send({ t: 'hello', protocol: app.cfg.protocolVersion, exe_timestamp: '', token: ticket });
    await c.next((m) => m.t === 'welcome');
    c.send({ t: 'renown_report', total: 1_500 });
    const r = await c.next((m) => m.t === 'renown');
    assert.equal(r.ok, true);
    assert.equal(r.renown, 1_500);
    assert.equal(app.director.channelOf(hero.id)?.players.get(hero.id)?.renown, 1_500);
    c.ws.close();
  } finally {
    server.close();
    app.close();
  }
});

class Fake implements Conn {
  got: ServerMsg[] = [];
  send(m: ServerMsg) { this.got.push(m); }
  close() {}
}

test('staff badge comes from the signed role, never from the client', () => {
  const secret = 's3cret';
  const hub = new ChatHub({ tokenSecret: secret, tagRadius: 50 });
  const login = (name: string, role: 'player' | 'mod' | 'admin') => {
    const c = new Fake();
    hub.open(c);
    hub.handle(c, { t: 'hello', v: 1, token: signToken({ sub: name, name, role, exp: Date.now() / 1000 + 60 }, secret) });
    hub.updateHero(name, { region: 1, x: 0, y: 0, z: 0 });
    return c;
  };
  const dev = login('Mobdon', 'admin');
  const gm = login('Warden', 'mod');
  const p = login('Sparrow', 'player');
  hub.tick();
  const welcome = (c: Fake) => c.got.find((m) => m.t === 'welcome') as Extract<ServerMsg, { t: 'welcome' }>;
  assert.equal(welcome(dev).staff, 'DEV');
  assert.equal(welcome(gm).staff, 'GM');
  assert.equal(welcome(p).staff, '');
  const tags = p.got.filter((m) => m.t === 'tags').flatMap((m) => (m as Extract<ServerMsg, { t: 'tags' }>).add);
  assert.equal(tags.find((t) => t.name === 'Mobdon')?.staff, 'DEV');
  assert.equal(tags.find((t) => t.name === 'Warden')?.staff, 'GM');
  // A player's hello cannot carry a badge: the dev block is ignored under token login.
  const sneaky = new Fake();
  hub.open(sneaky);
  hub.handle(sneaky, { t: 'hello', v: 1, token: signToken({ sub: 'x', name: 'Faker', role: 'player', exp: Date.now() / 1000 + 60 }, secret), dev: { title: 'DEV' } } as any);
  assert.equal(welcome(sneaky).staff, '');
});
