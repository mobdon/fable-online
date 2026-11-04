// The connection layer that turns stand-ins into real players: checked position reports,
// corrections, walk and run on the wire, reconnecting with a resume token, heartbeats.
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { link, testApp, makeHero } from './helpers.js';
import { listen } from '../src/net/server.js';
import { MOVE_RULES, MoveGuard, ANIM } from '../src/world/movement.js';
import { ResumeTokens } from '../src/net/resume.js';

const at = (x: number, y = 0, z = 0) => ({ x, y, z });

test('the move guard lets walking and running through and refuses teleports', () => {
  const g = new MoveGuard(0);
  let t = 0;
  let server = at(0);
  // Run at 7 m/s, reported every 100 ms, for three seconds.
  for (let i = 1; i <= 30; i++) {
    t += 100;
    const next = at(i * 0.7);
    const v = g.check(server, next, t);
    assert.ok(v.ok, `step ${i}`);
    server = next;
  }
  assert.equal(g.anim(t), ANIM.RUN);
  assert.equal(g.anim(t + 1000), ANIM.IDLE, 'no reports for a second means standing still');

  // 50 m in 100 ms is a teleport.
  t += 100;
  const v = g.check(server, at(server.x + 50), t);
  assert.deepEqual(v, { ok: false, reason: 'too_fast' });

  // A held burst is allowed once (a lag spike), not forever.
  const h = new MoveGuard(0);
  assert.ok(h.check(at(0), at(MOVE_RULES.maxSpeed), 1000).ok, 'a second of top speed after a second of silence');
  assert.equal(h.check(at(MOVE_RULES.maxSpeed), at(MOVE_RULES.maxSpeed * 2), 1010).ok, false, 'but not again 10 ms later');

  // Up a wall is refused; off a ledge is fine.
  const z = new MoveGuard(0);
  assert.deepEqual(z.check(at(0), at(0.5, 0, 20), 100), { ok: false, reason: 'too_high' });
  assert.ok(z.check(at(0), at(0.5, 0, -10), 1000).ok);
  assert.deepEqual(z.check(at(0), at(1e9), 1100), { ok: false, reason: 'out_of_bounds' });
});

test('an anchor lets exactly one far report through', () => {
  const g = new MoveGuard(0, Infinity);
  assert.deepEqual(g.check(at(0), at(400, 300, 12), 100), { ok: true, anchored: true, speed: 0 });
  assert.equal(g.check(at(400, 300, 12), at(0), 200).ok, false, 'the anchor is used up');

  const near = new MoveGuard(0, 40);
  assert.equal(near.check(at(0), at(100), 100).ok, false, 'an arrival anchor only covers the entrance');
  near.reanchor(40);
  assert.ok(near.check(at(0), at(30), 200).ok);
});

test('the channel keeps a refused hero where it was and says where that is', () => {
  const app = testApp({ joinAnchor: false });
  const { hero } = makeHero(app);
  app.db.run('UPDATE heroes SET x = 10, y = 10 WHERE id = ?', hero.id);
  const ch = app.director.join(app.heroes.get(hero.id)!, link(hero.id, 'Mob', []));

  const ok = ch.reportPosition(hero.id, at(10.5, 10), 0.3, Date.now() + 100);
  assert.ok(ok.verdict.ok);
  assert.deepEqual(ch.sim.position(hero.id), at(10.5, 10));

  const bad = ch.reportPosition(hero.id, at(200, 10), 0.3, Date.now() + 150);
  assert.equal(bad.verdict.ok, false);
  assert.deepEqual(bad.correct, at(10.5, 10));
  assert.deepEqual(ch.sim.position(hero.id), at(10.5, 10));
  // Corrections are throttled: the next refusal right after gets none.
  const again = ch.reportPosition(hero.id, at(200, 10), 0.3, Date.now() + 160);
  assert.equal(again.correct, undefined);
  app.close();
});

test('a hero reported walking shows as walking to everyone else', () => {
  const app = testApp({ joinAnchor: false });
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  const inbox: string[] = [];
  const ch = app.director.join(a, link(a.id, 'Watcher', inbox));
  app.director.join(b, link(b.id, 'Walker', []));
  let x = 0;
  const t0 = Date.now();
  for (let i = 1; i <= 5; i++) ch.reportPosition(b.id, at((x += 0.15)), 0, t0 + i * 100);
  // Real time has not moved, so the guard still counts the last report as fresh.
  ch.step(50);
  const tick = JSON.parse(inbox.at(-1)!);
  const walker = tick.entities.find((e: any) => e.id === b.id);
  assert.equal(walker.anim, ANIM.WALK);
  app.close();
});

test('resume tokens are one-time, per hero, and run out after the connection ends', () => {
  const r = new ResumeTokens(1000);
  const t1 = r.issue(7, 1);
  assert.ok(ResumeTokens.looksLike(t1));
  const t2 = r.issue(7, 1);
  assert.equal(r.redeem(t1), null, 'a newer token cancels the older one');
  assert.deepEqual(r.redeem(t2, 1e15), { heroId: 7, accountId: 1 }, 'no clock while connected');
  assert.equal(r.redeem(t2), null, 'used up');

  const t3 = r.issue(8, 2);
  r.disconnected(8, 0);
  assert.equal(r.redeem(t3, 5000), null, 'too late');
  const t4 = r.issue(8, 2);
  r.disconnected(8, 0);
  r.sweep(500);
  assert.deepEqual(r.redeem(t4, 900), { heroId: 8, accountId: 2 });
});

// ---------------------------------------------------------------- over real sockets

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
  const closed = new Promise<number>((r) => ws.on('close', (code) => r(code)));
  const next = (pred: (m: any) => boolean, ms = 3000) =>
    new Promise<any>((resolve, reject) => {
      const hit = inbox.find(pred);
      if (hit) return resolve(hit);
      waiters.push([pred, resolve]);
      setTimeout(() => reject(new Error('timed out waiting for a frame')), ms);
    });
  const send = (m: object) => ws.send(JSON.stringify(m));
  return { ws, open, closed, next, send, inbox };
}

async function serve(overrides = {}, heartbeatMs?: number) {
  const app = testApp({ tickRate: 30, ...overrides });
  const server = await listen(app, 0, heartbeatMs);
  const port = (server.address() as AddressInfo).port;
  app.director.start();
  const ticket = (heroId: number, accountId: number) => app.accounts.issueJoinTicket(accountId, heroId, app.cfg.realmId);
  return { app, url: `ws://127.0.0.1:${port}/play`, ticket, stop: () => (server.close(), app.close()) };
}

test('two real clients: anchors, walking, a refused teleport and the correction', async () => {
  const s = await serve();
  try {
    const one = makeHero(s.app);
    const two = makeHero(s.app);
    const a = connect(s.url);
    const b = connect(s.url);
    await Promise.all([a.open, b.open]);
    a.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(one.hero.id, one.acct.id) });
    b.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(two.hero.id, two.acct.id) });
    const welcome = await b.next((m) => m.t === 'welcome');
    assert.equal(welcome.client_move, 'checked');
    assert.ok(welcome.resume.startsWith('resume.'));
    await a.next((m) => m.t === 'welcome');

    // B's Fable loaded its save 30 m away from where the server had the hero: the first report anchors.
    b.send({ t: 'hero_pos', x: 30, y: 4, z: 0, yaw: 0.5, area: 1 });
    await a.next((m) => m.t === 'tick' && m.entities.some((e: any) => e.id === two.hero.id && Math.abs(e.x - 30) < 0.01));
    assert.ok(s.app.sink.events.some((e: any) => e.type === 'move_anchor' && e.heroId === two.hero.id));

    // Then B walks; A sees it.
    for (let i = 1; i <= 5; i++) {
      await new Promise((r) => setTimeout(r, 100));
      b.send({ t: 'hero_pos', x: 30 + i * 0.5, y: 4, z: 0, yaw: 0.5, area: 1 });
    }
    await a.next((m) => m.t === 'tick' && m.entities.some((e: any) => e.id === two.hero.id && Math.abs(e.x - 32.5) < 0.01));

    // A teleport is refused, B is told where the server has it, A never sees the jump.
    b.send({ t: 'hero_pos', x: 300, y: 4, z: 0, area: 1 });
    const fix = await b.next((m) => m.t === 'pos_correct');
    assert.ok(Math.abs(fix.x - 32.5) < 0.01);
    assert.equal(fix.reason, 'too_fast');
    await new Promise((r) => setTimeout(r, 100));
    assert.ok(!a.inbox.some((m) => m.t === 'tick' && m.entities.some((e: any) => e.id === two.hero.id && e.x > 100)));
    assert.ok(s.app.sink.events.some((e: any) => e.type === 'move_rejected' && e.reason === 'too_fast'));

    a.ws.close();
    b.ws.close();
  } finally {
    s.stop();
  }
});

test('a dropped connection comes back with its resume token, once', async () => {
  const s = await serve();
  try {
    const { hero, acct } = makeHero(s.app);
    const c = connect(s.url);
    await c.open;
    c.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(hero.id, acct.id) });
    const w = await c.next((m) => m.t === 'welcome');
    c.send({ t: 'hero_pos', x: 12, y: 3, z: 0, area: 1 });
    await c.next((m) => m.t === 'tick' && m.entities.some((e: any) => e.id === hero.id && Math.abs(e.x - 12) < 0.01));
    c.ws.terminate();
    await new Promise((r) => setTimeout(r, 100));
    assert.equal(s.app.director.channelOf(hero.id), undefined, 'the hero left the world');

    // The same ticket is spent...
    const again = connect(s.url);
    await again.open;
    again.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: 'not-a-ticket' });
    assert.equal((await again.next((m) => m.t === 'error')).error, 'bad_token');

    // ...but the resume token brings the hero back where they were.
    const r = connect(s.url);
    await r.open;
    r.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: w.resume });
    const w2 = await r.next((m) => m.t === 'welcome');
    assert.equal(w2.player_id, hero.id);
    assert.notEqual(w2.resume, w.resume, 'a fresh token for the next drop');
    const back = await r.next((m) => m.t === 'tick' && m.entities.some((e: any) => e.id === hero.id));
    assert.ok(Math.abs(back.entities.find((e: any) => e.id === hero.id).x - 12) < 0.01);
    assert.ok(s.app.sink.events.some((e: any) => e.type === 'player_resumed'));
    // A resumed session does not get a fresh anchor: dropping the link is no way to teleport.
    r.send({ t: 'hero_pos', x: 200, y: 3, z: 0, area: 1 });
    assert.equal((await r.next((m) => m.t === 'pos_correct')).reason, 'too_fast');

    // The old token is gone.
    const twice = connect(s.url);
    await twice.open;
    twice.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: w.resume });
    assert.equal((await twice.next((m) => m.t === 'error')).error, 'resume_expired');
    r.ws.close();
  } finally {
    s.stop();
  }
});

test('a client that stops answering pings is dropped and its hero leaves', async () => {
  const s = await serve({}, 100);
  try {
    const { hero, acct } = makeHero(s.app);
    const c = connect(s.url);
    await c.open;
    c.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(hero.id, acct.id) });
    await c.next((m) => m.t === 'welcome');
    // A frozen game: the socket stays open but nothing answers.
    (c.ws as any)._socket.pause();
    c.ws.pong = () => {};
    await new Promise((r) => setTimeout(r, 400));
    assert.equal(s.app.director.channelOf(hero.id), undefined);
    c.ws.terminate();
  } finally {
    s.stop();
  }
});

test('with client movement off, reports are ignored', async () => {
  const s = await serve({ clientMovement: 'off' });
  try {
    const { hero, acct } = makeHero(s.app);
    const c = connect(s.url);
    await c.open;
    c.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(hero.id, acct.id) });
    assert.equal((await c.next((m) => m.t === 'welcome')).client_move, 'off');
    c.send({ t: 'hero_pos', x: 1, y: 0, z: 0, area: 1 });
    await new Promise((r) => setTimeout(r, 150));
    assert.deepEqual(s.app.director.channelOf(hero.id)!.sim.position(hero.id), at(0));
    c.ws.close();
  } finally {
    s.stop();
  }
});

test('test realms can start new heroes in the shared world', () => {
  const app = testApp({ newHeroPhase: 'world' });
  const acct = app.accounts.register('starter_acct', 'password123');
  const hero = app.heroes.create(acct.id, 'Starter');
  assert.equal(hero.phase, 'world');
  assert.equal(hero.region, 'HeroGuildComplex');
  app.close();
});

test('an account with no hero gets one named after it on its first join', async () => {
  const s = await serve({ newHeroPhase: 'world', autoHero: true });
  try {
    const base = s.url.replace('ws://', 'http://').replace('/play', '');
    const post = async (path: string, body: object, token?: string) =>
      (await fetch(base + path, {
        method: 'POST',
        headers: { 'content-type': 'application/json', ...(token ? { authorization: `Bearer ${token}` } : {}) },
        body: JSON.stringify(body),
      })).json() as Promise<any>;
    const auth = await post('/launcher/v1/register', { username: 'sparrow_7', password: 'password123' });
    const join = await post('/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, auth.access_token);
    assert.equal(join.hero.name, 'Sparrow');
    assert.ok(join.ticket);
    // The second join plays the same hero.
    const again = await post('/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, auth.access_token);
    assert.equal(again.hero.id, join.hero.id);
    const m = await (await fetch(base + '/launcher/v1/manifest')).json() as any;
    assert.deepEqual(m.files, [], 'no manifest published: nothing to patch, and the launcher may play');
  } finally {
    s.stop();
  }
});

test('an area the client loaded by itself lets the jump through once, and not too often', () => {
  const g = new MoveGuard(0);
  assert.equal(g.noteArea(111, 0), 'same', 'the first area is where the hero already is');
  assert.equal(g.check(at(0), at(500), 100).ok, false, 'no jump without an area change');
  assert.equal(g.noteArea(222, 5_000), 'changed');
  assert.ok(g.check(at(0), at(3947, 4286, 20), 5_100).ok, 'out of the town into the next area');
  assert.equal(g.check(at(3947, 4286, 20), at(0), 5_200).ok, false, 'one jump per change');
  // Flicking between areas faster than Fable can load them is not believed.
  assert.equal(g.noteArea(333, 6_000), 'too_soon');
  assert.equal(g.noteArea(333, 6_100), 'same', 'reported once, then quiet until the wait is over');
  assert.equal(g.area, 222);
  assert.equal(g.noteArea(333, 8_100), 'changed');
  assert.equal(g.noteArea(0, 9_000), 'same', 'an unknown area changes nothing');
});

test('heroes in different areas of one channel do not see each other', () => {
  const app = testApp({ joinAnchor: false });
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  const framesB: string[] = [];
  const ch = app.director.join(app.heroes.get(a.id)!, link(a.id, 'A', []));
  app.director.join(app.heroes.get(b.id)!, link(b.id, 'B', framesB));
  const ids = () => {
    framesB.length = 0;
    ch.step(50);
    const tick = framesB.map((f) => JSON.parse(f)).filter((f) => f.t === 'tick').pop();
    return (tick.entities as { id: number }[]).map((e) => e.id);
  };
  assert.ok(ids().includes(a.id));
  ch.moves.get(a.id)!.noteArea(1, 0);
  ch.moves.get(b.id)!.noteArea(1, 0);
  assert.ok(ids().includes(a.id), 'same area');
  ch.moves.get(a.id)!.noteArea(2, 10_000);
  assert.ok(!ids().includes(a.id), 'A walked into another area');
  assert.ok(ids().includes(b.id), 'B still sees itself');
  app.close();
});

test('a hero who keeps moving normally after a refused jump is taken to be there', () => {
  const g = new MoveGuard(0);
  assert.ok(g.check(at(0), at(1), 100).ok);
  let t = 100;
  // Fable loaded the next area without the mod noticing: reports now come from 3 km away.
  const far = (i: number) => at(3000 + i * 0.5, 10);
  let accepted = -1;
  for (let i = 0; i < 40 && accepted < 0; i++) {
    t += 100;
    const v = g.check(at(1), far(i), t);
    if (v.ok) accepted = i;
  }
  assert.ok(accepted >= 19 && accepted <= 22, `settled after about two seconds (report ${accepted})`);
  // A single teleport, or one that keeps hopping, never settles.
  const h = new MoveGuard(0);
  for (let i = 1; i <= 40; i++) assert.equal(h.check(at(0), at(i % 2 ? 3000 : -3000), i * 100).ok, false);
});

test('a load the mod saw lets one jump through, at most every few seconds', () => {
  const g = new MoveGuard(0);
  assert.equal(g.noteLoad(10_000), 'changed');
  assert.ok(g.check(at(0), at(2500), 10_100).ok);
  assert.equal(g.noteLoad(11_000), 'too_soon');
  assert.equal(g.check(at(2500), at(0), 11_100).ok, false);
});

test('the server follows a hero into the region their own Fable loaded', async () => {
  const fs = await import('node:fs');
  const os = await import('node:os');
  const path = await import('node:path');
  const file = path.join(fs.mkdtempSync(path.join(os.tmpdir(), 'fo-regions-')), 'regions.json');
  fs.writeFileSync(
    file,
    JSON.stringify({
      maps: [
        { uid: 1, script: 'HeroGuildComplex', sea: false, region: 'HeroGuildComplexInside', x: 4576, y: 3552, w: 256, h: 224 },
        { uid: 2, script: 'BowerstoneSlums_v2', sea: false, region: 'BowerstoneSlums', x: 3744, y: 4224, w: 256, h: 160 },
        { uid: 3, script: 'Filler_X', sea: false, region: 'Filler_X', x: 0, y: 0, w: 100, h: 100 },
      ],
      graph: { HeroGuildComplexInside: ['LookoutPoint'], BowerstoneSlums: ['LookoutPoint'], LookoutPoint: ['HeroGuildComplex', 'BowerstoneSlums'] },
    }),
  );
  const app = testApp({ regionsPath: file });
  assert.equal(app.regions.locate(3947, 4286), 'BowerstoneSlums');
  assert.equal(app.regions.locate(50, 50), undefined, 'scenery levels are not regions');
  const { hero } = makeHero(app);
  const frames: string[] = [];
  const ch = app.director.join(app.heroes.get(hero.id)!, link(hero.id, 'Mob', frames));
  assert.equal(ch.region.name, 'HeroGuildComplex');
  assert.equal(app.director.follow(hero.id, at(4600, 3600)), undefined, 'the Guild by its level name is the same place');
  // The save loaded in Bowerstone: the server moves the hero there, with no load on the client.
  const to = app.director.follow(hero.id, at(3947, 4286, 20));
  assert.equal(to?.region.name, 'BowerstoneSlums');
  assert.equal(app.director.channelOf(hero.id)?.region.name, 'BowerstoneSlums');
  const enter = frames.map((f) => JSON.parse(f)).filter((f) => f.t === 'enter_region').pop();
  assert.equal(enter.region, 'BowerstoneSlums');
  assert.equal(enter.loading, false);
  assert.equal(app.heroes.get(hero.id)!.region, 'BowerstoneSlums', 'saved for the next login');
  // Reports go on from there without a correction.
  const next = to!.reportPosition(hero.id, at(3947.5, 4286, 20), 0, Date.now() + 100);
  assert.ok(next.verdict.ok);
  assert.equal(app.director.follow(hero.id, at(3948, 4286, 20)), undefined, 'same region: nothing to do');
  app.close();
});

test('a banned account cannot come back on a resume token', async () => {
  const s = await serve();
  try {
    const { hero, acct } = makeHero(s.app);
    const c = connect(s.url);
    await c.open;
    c.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: s.ticket(hero.id, acct.id) });
    const w = await c.next((m) => m.t === 'welcome');
    c.ws.terminate();
    await new Promise((r) => setTimeout(r, 100));
    s.app.db.run('UPDATE accounts SET banned_until = ? WHERE id = ?', Date.now() + 3600_000, acct.id);
    const r = connect(s.url);
    await r.open;
    r.send({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: w.resume });
    assert.equal((await r.next((m) => m.t === 'error')).error, 'banned');
  } finally {
    s.stop();
  }
});
