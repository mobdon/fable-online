// A hero's swing, sent as input buttons by the mod, hurts another player's hero when the
// realm allows PvP (FO_PVP_ALL), and does nothing when it does not.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { testApp } from './helpers.js';
import { listen } from '../src/net/server.js';

function connect(url: string) {
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  ws.on('message', (d) => inbox.push(JSON.parse(d.toString())));
  const open = new Promise<void>((r) => ws.on('open', () => r()));
  return { ws, open, inbox };
}

const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));

async function until<T>(f: () => T | undefined, ms = 3000): Promise<T> {
  const end = Date.now() + ms;
  for (;;) {
    const v = f();
    if (v !== undefined) return v;
    if (Date.now() > end) throw new Error('timed out');
    await wait(20);
  }
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

function latest(inbox: any[], id: number) {
  for (let i = inbox.length - 1; i >= 0; i--) {
    const m = inbox[i];
    if (m.t === 'tick') return m.entities.find((e: any) => e.id === id);
  }
  return undefined;
}

async function swingAt(pvpEverywhere: boolean) {
  const app = testApp({ tickRate: 30, clientMovement: 'trusted', interestRadius: 80, pvpEverywhere });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Walker');
    const a = connect(`ws://127.0.0.1:${port}/play`);
    const b = connect(`ws://127.0.0.1:${port}/play`);
    await Promise.all([a.open, b.open]);
    a.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    b.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket }));
    await until(() => (a.inbox.some((m) => m.t === 'welcome') && b.inbox.some((m) => m.t === 'welcome') ? true : undefined));

    // The mod's yaw convention: forward = (-sin yaw, cos yaw), so -pi/2 faces +x.
    a.ws.send(JSON.stringify({ t: 'hero_pos', x: 100, y: 100, z: 0, yaw: -Math.PI / 2, area: 0 }));
    b.ws.send(JSON.stringify({ t: 'hero_pos', x: 101, y: 100, z: 0, yaw: Math.PI / 2, area: 0 }));
    const victim = () => {
      for (let i = a.inbox.length - 1; i >= 0; i--) {
        const m = a.inbox[i];
        if (m.t === 'tick') return m.entities.find((e: any) => e.id === two.hero.id);
      }
      return undefined;
    };
    const before = await until(() => victim());
    await wait(200);
    // What report_combat sends: attack down, then up (the server swings on release).
    a.ws.send(JSON.stringify({ t: 'input', seq: 1, buttons: 1, camera_yaw: -Math.PI / 2 }));
    await wait(100);
    a.ws.send(JSON.stringify({ t: 'input', seq: 2, buttons: 0, camera_yaw: -Math.PI / 2 }));
    await wait(150);
    // What the other player's mod is told to play on each stand-in right after the swing.
    const swinger = latest(b.inbox, one.hero.id);
    const flinch = victim();
    await wait(250);
    const after = victim();
    a.ws.close();
    b.ws.close();
    return { before: before.health, after: after.health, swingAnim: swinger?.anim, hitAnim: flinch?.anim };
  } finally {
    app.director.stop();
    server.close();
  }
}

test('with FO_PVP_ALL a swing from the mod hurts the hero in front', async () => {
  const r = await swingAt(true);
  assert.ok(r.after < r.before, `health ${r.before} -> ${r.after}`);
  assert.equal(r.swingAnim, 3, 'others see the swing (Anim.Attack)');
  assert.equal(r.hitAnim, 5, 'others see the flinch (Anim.Hit)');
});

test('without PvP the same swing does nothing', async () => {
  const r = await swingAt(false);
  assert.equal(r.after, r.before);
  assert.equal(r.swingAnim, 3, 'the swing still shows');
});
