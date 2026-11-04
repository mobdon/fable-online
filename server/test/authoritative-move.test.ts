// Authoritative movement: the server moves heroes from the stick alone. A position report
// only anchors the hero after a join (the client's Fable loads its own save, which is not
// where the server last had the hero); every later report is ignored. A stick the server
// stops hearing is let go, and heroes slide along the region's navmesh.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { testApp } from './helpers.js';
import { listen } from '../src/net/server.js';
import { CombatSim, INPUT_HOLD_MS } from '../src/world/combat-sim.js';
import { FALLBACK_DATA } from '../src/data/game-data.js';
import { MemorySink } from '../src/analytics/events.js';
import { asciiNav } from './nav-fixture.js';

const sleep = (ms: number) => new Promise((r) => setTimeout(r, ms));

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

test('anchor mode: the first report places the hero, later reports are ignored, input moves it, a quiet stick stops it', async () => {
  const app = testApp({ tickRate: 30, clientMovement: 'anchor', heroRunSpeed: 7 });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Anchor');
    const c = connect(`ws://127.0.0.1:${port}/play`);
    await c.open;
    c.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    const w = await c.next((m) => m.t === 'welcome');
    assert.equal(w.client_move, 'anchor');
    assert.equal(w.run_speed, 7);
    await c.next((m) => m.t === 'tick');
    const me = () => c.inbox.filter((m) => m.t === 'tick').at(-1).entities.find((e: any) => e.id === one.hero.id);

    // The client's Fable loaded its save at (120, 80): the first report anchors the server there.
    c.ws.send(JSON.stringify({ t: 'hero_pos', x: 120, y: 80, z: 0, area: 1 }));
    await sleep(150);
    assert.ok(Math.abs(me().x - 120) < 0.5 && Math.abs(me().y - 80) < 0.5, `anchored at ${me().x},${me().y}`);

    // A second report, far away, is not a move: ignored, and not refused either.
    c.ws.send(JSON.stringify({ t: 'hero_pos', x: 500, y: 500, z: 0, area: 1 }));
    await sleep(150);
    assert.ok(Math.abs(me().x - 120) < 0.5 && Math.abs(me().y - 80) < 0.5, 'the hero stayed where the server has it');
    assert.ok(!c.inbox.some((m) => m.t === 'pos_correct'), 'no correction was sent');

    // Input moves it: a full stick forward with yaw 0 is +y at the run speed, kept alive by repeats.
    const t0 = Date.now();
    let seq = 1;
    const timer = setInterval(
      () => c.ws.send(JSON.stringify({ t: 'input', seq: seq++, move_x: 0, move_y: 1, buttons: 0, camera_yaw: 0 })),
      100,
    );
    await sleep(1000);
    clearInterval(timer);
    const secs = (Date.now() - t0) / 1000;
    const moved = me().y - 80;
    assert.ok(moved > 7 * secs * 0.5 && moved < 7 * secs * 1.1 + 0.5, `moved ${moved.toFixed(2)} m in ${secs.toFixed(2)} s`);

    // With the stick no longer heard, the hero stops within the hold time.
    await sleep(INPUT_HOLD_MS + 250);
    const stopped = me().y;
    await sleep(300);
    assert.ok(Math.abs(me().y - stopped) < 0.01, 'the hero stopped when the stick went quiet');
    c.ws.close();
  } finally {
    server.close();
    app.close();
  }
});

test('combat sim: heroes slide along the navmesh and stay on the ground', () => {
  // A corridor 3 m wide along y, walls on both sides, floor at 10 m.
  const nav = asciiNav(Array.from({ length: 12 }, () => '#...#'), () => 10);
  const sim = new CombatSim({ name: 'TestRegion', uid: 1, sea: false, neighbours: [] }, 'TestRegion#1', {
    data: FALLBACK_DATA,
    spawns: { regions: { TestRegion: { creatures: [], generators: [] } } },
    events: new MemorySink(),
    nav: () => nav,
    heroRunSpeed: 5,
  });
  sim.addHero(1, { x: 2.5, y: 2, z: 0 });
  const push = (seq: number, mx: number, my: number) => sim.applyInput(1, { seq, moveX: mx, moveY: my, buttons: 0, cameraYaw: 0 });

  // A full stick to the right (+x) runs into the wall at x = 4 and is held there.
  for (let t = 0; t < 1000; t += 50) {
    push(1 + t, 1, 0);
    sim.step(50);
  }
  const held = sim.position(1)!;
  assert.ok(held.x < 4 && held.x > 2.5, `held by the wall at x=${held.x.toFixed(2)}`);
  assert.equal(Math.round(held.z), 10, 'on the ground');

  // Diagonally into the wall slides along it: y still advances.
  for (let t = 0; t < 1000; t += 50) {
    push(2000 + t, 1, 1);
    sim.step(50);
  }
  assert.ok(sim.position(1)!.y > held.y + 1, 'slid along the wall');
  assert.ok(sim.position(1)!.x < 4, 'still inside the corridor');
  sim.dispose();
});
