// Two players in one region see each other's heroes in their own frames. This is the
// server half of the first two-player test: the mod turns these frames into the stand-in
// hero the other player walks around as.
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
      setTimeout(() => reject(new Error('timed out waiting for a frame')), 3000);
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

test('two players in a region see each other, named, and the mod can report its position', async () => {
  // Position reports are a dev path, so this test has to turn them on explicitly.
  const app = testApp({ tickRate: 30, clientMovement: 'trusted', interestRadius: 80 });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Scythe');

    const a = connect(`ws://127.0.0.1:${port}/play`);
    const b = connect(`ws://127.0.0.1:${port}/play`);
    await Promise.all([a.open, b.open]);
    a.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    b.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket }));
    await Promise.all([a.next((m) => m.t === 'welcome'), b.next((m) => m.t === 'welcome')]);

    // Both heroes graduate into the same region, so each should show up for the other.
    const frame = await a.next((m) => m.t === 'tick' && m.entities.length === 2);
    const other = frame.entities.find((e: any) => e.id === two.hero.id);
    assert.equal(other.kind, 'hero');
    assert.equal(other.name, 'Scythe');

    // The second player walks 20 m away; the first player's frames follow them there.
    b.ws.send(JSON.stringify({ t: 'hero_pos', x: 20, y: 5, z: 1, yaw: 1.2, area: 7 }));
    const moved = await a.next(
      (m) => m.t === 'tick' && m.entities.some((e: any) => e.id === two.hero.id && Math.abs(e.x - 20) < 0.01),
    );
    const there = moved.entities.find((e: any) => e.id === two.hero.id);
    assert.ok(Math.abs(there.y - 5) < 0.01);
    assert.ok(Math.abs(there.yaw - 1.2) < 0.01);

    // Out past the interest radius they drop out of each other's frames.
    b.ws.send(JSON.stringify({ t: 'hero_pos', x: 400, y: 0, z: 0, area: 7 }));
    const alone = await a.next((m) => m.t === 'tick' && m.entities.length === 1);
    assert.equal(alone.entities[0].id, one.hero.id);

    a.ws.close();
    b.ws.close();
    await new Promise((r) => setTimeout(r, 100));
  } finally {
    server.close();
    app.close();
  }
});

test('with the dev path off, a client cannot move its own hero', async () => {
  const app = testApp({ tickRate: 30, clientMovement: 'off' });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Thunder');
    const c = connect(`ws://127.0.0.1:${port}/play`);
    await c.open;
    c.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    await c.next((m) => m.t === 'welcome');
    await c.next((m) => m.t === 'tick');

    c.ws.send(JSON.stringify({ t: 'hero_pos', x: 500, y: 500, z: 0, area: 1 }));
    await new Promise((r) => setTimeout(r, 200));
    const last = c.inbox.filter((m) => m.t === 'tick').at(-1);
    const me = last.entities.find((e: any) => e.id === one.hero.id);
    assert.ok(Math.abs(me.x) < 1 && Math.abs(me.y) < 1, 'the hero stayed where the server put it');
    assert.ok(app.sink.events.some((e: any) => e.type === 'input_rejected'), 'the attempt was recorded');
    c.ws.close();
  } finally {
    server.close();
    app.close();
  }
});
