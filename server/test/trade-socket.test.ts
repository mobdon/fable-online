// Two players trade over the game socket: invite, offers, both accept, and the ledger swap.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { testApp } from './helpers.js';
import { listen } from '../src/net/server.js';

function connect(url: string) {
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  const waiters: [(m: any) => boolean, (m: any) => void][] = [];
  ws.on('message', (d) => {
    const m = JSON.parse(d.toString());
    inbox.push(m);
    for (const w of [...waiters])
      if (w[0](m)) {
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
      setTimeout(() => reject(new Error("timed out waiting for a frame; got " + inbox.map((m) => m.t + (m.t === "trade_error" || m.t === "error" ? ":" + m.error : "")).filter((t) => t !== "tick").join(","))), 3000);
    });
  return { ws, open, next, inbox };
}

async function joinAs(app: any, base: string, name: string) {
  const reg = await fetch(base + '/launcher/v1/register', {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ username: name, password: 'password123' }),
  });
  const tok = ((await reg.json()) as any).access_token;
  const made = await fetch(base + '/launcher/v1/heroes', {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${tok}` },
    body: JSON.stringify({ name }),
  });
  const hero = ((await made.json()) as any).hero;
  app.heroes.advancePhase(hero.id, 'guild_training');
  app.heroes.advancePhase(hero.id, 'world');
  const join = await fetch(base + '/launcher/v1/join', {
    method: 'POST',
    headers: { 'content-type': 'application/json', authorization: `Bearer ${tok}` },
    body: JSON.stringify({ region: 'albion-1', client_version: '0.1.0' }),
  });
  return { hero, ticket: ((await join.json()) as any).ticket };
}

test('two players trade over the game socket', async () => {
  const app = testApp({ tickRate: 30, clientMovement: 'trusted', interestRadius: 80 });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Scythe');
    app.economy.grant(one.hero.id, 700, 'quest_reward', 'test-grant-1');
    const potions = app.economy.mintItem(two.hero.id, 'HealthPotion', 5, 'loot', 'test-mint-1').itemId;

    const a = connect(`ws://127.0.0.1:${port}/play`);
    const b = connect(`ws://127.0.0.1:${port}/play`);
    await Promise.all([a.open, b.open]);
    a.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    b.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket }));
    await Promise.all([a.next((m) => m.t === 'welcome'), b.next((m) => m.t === 'welcome')]);
    // Both heroes arrive at the region's entrance, side by side.
    await a.next((m) => m.t === 'tick' && m.entities.length === 2);

    a.ws.send(JSON.stringify({ t: 'trade_request', to: two.hero.id }));
    const inv = await b.next((m) => m.t === 'trade_invite');
    assert.equal(inv.name, 'Mobdon');
    b.ws.send(JSON.stringify({ t: 'trade_answer', from: one.hero.id, yes: true }));
    const opened = await b.next((m) => m.t === 'trade_state' && m.stock);
    assert.deepEqual(opened.stock, [{ item: potions, def: 'HealthPotion', qty: 5 }]);

    a.ws.send(JSON.stringify({ t: 'trade_offer', gold: 700, items: [] }));
    b.ws.send(JSON.stringify({ t: 'trade_offer', gold: 0, items: [{ id: potions, qty: 2 }] }));
    const seen = await a.next((m) => m.t === 'trade_state' && m.theirs.items.length === 1 && m.mine.gold === 700);
    await new Promise((r) => setTimeout(r, seen.settle_ms + 50));
    a.ws.send(JSON.stringify({ t: 'trade_accept', version: seen.version }));
    b.ws.send(JSON.stringify({ t: 'trade_accept', version: seen.version }));
    const [da, db] = await Promise.all([a.next((m) => m.t === 'trade_done'), b.next((m) => m.t === 'trade_done')]);
    assert.equal(da.gold, 0);
    assert.equal(db.gold, 700);
    assert.equal(app.economy.countOf(one.hero.id, 'HealthPotion'), 2);
    assert.equal(app.economy.countOf(two.hero.id, 'HealthPotion'), 3);
    assert.ok(app.economy.audit().ok);
    a.ws.close();
    b.ws.close();
  } finally {
    server.close();
    app.close();
  }
});
