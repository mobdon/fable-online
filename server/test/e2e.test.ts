import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { testApp } from './helpers.js';
import { listen } from '../src/net/server.js';

async function api(base: string, method: string, path: string, body?: unknown, token?: string) {
  const res = await fetch(base + path, {
    method,
    headers: { 'content-type': 'application/json', ...(token ? { authorization: `Bearer ${token}` } : {}) },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return { status: res.status, body: (await res.json()) as any };
}

function connect(url: string) {
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  const waiters: [(m: any) => boolean, (m: any) => void][] = [];
  ws.on('message', (d) => {
    const m = JSON.parse(d.toString());
    inbox.push(m);
    for (const w of [...waiters]) if (w[0](m)) { waiters.splice(waiters.indexOf(w), 1); w[1](m); }
  });
  const closed = new Promise<number>((r) => ws.on('close', (c) => r(c)));
  const open = new Promise<void>((r) => ws.on('open', () => r()));
  const next = (pred: (m: any) => boolean) =>
    new Promise<any>((resolve, reject) => {
      const hit = inbox.find(pred);
      if (hit) return resolve(hit);
      waiters.push([pred, resolve]);
      setTimeout(() => reject(new Error('timeout')), 3000);
    });
  return { ws, open, next, closed, inbox };
}

test('launcher login -> join ticket -> game socket -> server-simulated movement', async () => {
  const app = testApp({ tickRate: 50 });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    assert.equal((await api(base, 'GET', '/launcher/v1/status')).body.regions[0].id, 'albion-1');
    const reg = await api(base, 'POST', '/launcher/v1/register', { username: 'Reaver', password: 'password123' });
    assert.equal(reg.status, 200);
    assert.ok(reg.body.access_token && reg.body.refresh_token);
    assert.equal(reg.body.account.name, 'Reaver');
    assert.equal((await api(base, 'POST', '/launcher/v1/register', { username: 'reaver', password: 'password123' })).body.error, 'name_taken');
    const login = await api(base, 'POST', '/launcher/v1/login', { username: 'Reaver', password: 'password123' });
    assert.equal(login.status, 200);
    const tok = login.body.access_token;
    assert.equal((await api(base, 'POST', '/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, tok)).body.error, 'no_hero');
    const hero = (await api(base, 'POST', '/launcher/v1/heroes', { name: 'Reaver' }, tok)).body.hero;
    app.heroes.advancePhase(hero.id, 'guild_training');
    app.heroes.advancePhase(hero.id, 'world');
    const old = await api(base, 'POST', '/launcher/v1/join', { client_version: '0.0.9' }, tok);
    assert.deepEqual([old.status, old.body.error], [409, 'outdated']);
    const join = await api(base, 'POST', '/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, tok);
    assert.equal(join.status, 200);
    assert.ok(join.body.ticket);

    const c = connect(`ws://127.0.0.1:${port}/play`);
    await c.open;
    c.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: join.body.ticket }));
    const welcome = await c.next((m) => m.t === 'welcome');
    assert.equal(welcome.player_id, hero.id);
    const enter = await c.next((m) => m.t === 'enter_region');
    assert.equal(enter.region, 'HeroGuildComplex');
    // A cheating client tries to run at 50x speed. The server clamps the stick to length 1.
    c.ws.send(JSON.stringify({ t: 'input', seq: 1, move_x: 0, move_y: 50, buttons: 0, camera_yaw: 0, x: 9999 }));
    await c.next((m) => m.t === 'tick' && m.last_input_seq === 1);
    await new Promise((r) => setTimeout(r, 500));
    const last = c.inbox.filter((m) => m.t === 'tick').at(-1);
    const y = last.entities[0].y;
    assert.ok(y > 1 && y < 6 * 0.7, `moved ${y} m in ~0.5 s`);
    assert.ok(last.entities[0].x < 1);

    // The ticket was single use.
    const c2 = connect(`ws://127.0.0.1:${port}/play`);
    await c2.open;
    c2.ws.send(JSON.stringify({ t: 'hello', protocol: 1, token: join.body.ticket }));
    assert.equal((await c2.next((m) => m.t === 'error')).error, 'bad_token');
    assert.equal(await c2.closed, 4000);

    // Garbage frames get you disconnected.
    c.ws.send('not json');
    assert.equal(await c.closed, 4002);
    await new Promise((r) => setTimeout(r, 50));
    assert.equal(app.director.online, 0);
    assert.ok(app.heroes.get(hero.id)!.y > 1, 'position saved on disconnect');

    const st = (await api(base, 'GET', '/launcher/v1/status')).body;
    assert.deepEqual([st.online, st.protocol, st.regions[0].status], [true, 1, 'online']);
    const out = await fetch(base + '/launcher/v1/logout', { method: 'POST', headers: { authorization: `Bearer ${tok}` } });
    assert.equal(out.status, 204);
    assert.equal((await api(base, 'POST', '/launcher/v1/refresh', { refresh_token: login.body.refresh_token })).status, 401);
  } finally {
    server.close();
    app.close();
  }
});
