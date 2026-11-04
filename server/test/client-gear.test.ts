// The weapon equipped in the hero's own Fable, reported by the mod, is what the server's
// combat fights with: only when the server accepts it (ledger, or a trusting test realm).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import type { AddressInfo } from 'node:net';
import { WebSocket } from 'ws';
import { FALLBACK_DATA as D } from '../src/data/game-data.js';
import { ClientGear, type GearRules } from '../src/combat/client-gear.js';
import { CombatSim } from '../src/world/combat-sim.js';
import { decode, ProtocolError } from '../src/net/protocol.js';
import { listen } from '../src/net/server.js';
import { key, makeHero, testApp } from './helpers.js';

const REGION = { name: 'TestRegion', uid: 1, sea: false, neighbours: [] };
const DATA = {
  ...D,
  items: { ...D.items, OBJECT_STAFF_TEST_SWORD: { value: 1, weapon: { type: 'sword' as const, class: 'light' as const, damage: 50 } } },
};

function gear(over: Partial<GearRules> = {}) {
  const owned = new Set<string>();
  const g = new ClientGear({
    data: DATA,
    trust: false,
    owns: (_h, d) => owned.has(d),
    staffOnly: (d) => d === 'OBJECT_STAFF_TEST_SWORD',
    isStaff: (h) => h === 99,
    ...over,
  });
  return { g, owned };
}

test('gear frame is checked', () => {
  assert.deepEqual(decode('{"t":"gear","melee":"OBJECT_IRON_LONGSWORD","ranged":""}'), { t: 'gear', melee: 'OBJECT_IRON_LONGSWORD', ranged: '' });
  assert.deepEqual(decode('{"t":"gear"}'), { t: 'gear', melee: '', ranged: '' });
  for (const bad of ['{"t":"gear","melee":"lower"}', '{"t":"gear","melee":5}', `{"t":"gear","ranged":"${'A'.repeat(65)}"}`])
    assert.throws(() => decode(bad), ProtocolError);
});

test('a reported weapon counts only if the ledger holds it, unless the realm trusts client gear', () => {
  const { g, owned } = gear();
  let r = g.report(1, { melee: 'OBJECT_IRON_LONGSWORD', ranged: '' });
  assert.deepEqual(r.refused, [{ def: 'OBJECT_IRON_LONGSWORD', reason: 'not_owned' }]);
  assert.deepEqual(g.equipped(1), []);
  assert.equal(r.changed, false);
  owned.add('OBJECT_IRON_LONGSWORD');
  r = g.report(1, { melee: 'OBJECT_IRON_LONGSWORD', ranged: '' });
  assert.equal(r.changed, true);
  assert.deepEqual(g.equipped(1), ['OBJECT_IRON_LONGSWORD']);
  assert.equal(g.report(1, { melee: 'OBJECT_IRON_LONGSWORD', ranged: '' }).changed, false, 'same report, nothing to redo');

  const trusted = gear({ trust: true }).g;
  r = trusted.report(1, { melee: 'OBJECT_IRON_WARHAMMER', ranged: 'OBJECT_YEW_LONGBOW' });
  assert.deepEqual(r.accepted, ['OBJECT_IRON_WARHAMMER', 'OBJECT_YEW_LONGBOW'], 'melee first');
  r = trusted.report(1, { melee: '', ranged: '' });
  assert.equal(r.changed, true);
  assert.deepEqual(trusted.equipped(1), [], 'sheathing nothing: unarmed again');
});

test('unknown items, non-weapons, wrong slots and staff gear are refused even when trusted', () => {
  const { g } = gear({ trust: true });
  const r = g.report(1, { melee: 'OBJECT_NOT_IN_DATA', ranged: 'OBJECT_IRON_LONGSWORD' });
  assert.deepEqual(r.refused, [
    { def: 'OBJECT_NOT_IN_DATA', reason: 'unknown_item' },
    { def: 'OBJECT_IRON_LONGSWORD', reason: 'wrong_slot' },
  ]);
  assert.deepEqual(g.report(1, { melee: 'OBJECT_HEALTH_POTION', ranged: '' }).refused, [{ def: 'OBJECT_HEALTH_POTION', reason: 'not_a_weapon' }]);
  assert.deepEqual(g.report(1, { melee: 'OBJECT_STAFF_TEST_SWORD', ranged: '' }).refused, [{ def: 'OBJECT_STAFF_TEST_SWORD', reason: 'staff_only' }]);
  assert.deepEqual(g.report(99, { melee: 'OBJECT_STAFF_TEST_SWORD', ranged: '' }).accepted, ['OBJECT_STAFF_TEST_SWORD'], 'staff may');
});

test('worn clothing: checked like weapons, replaces the ledger clothing once reported', () => {
  const { g, owned } = gear();
  let r = g.report(1, { melee: '', ranged: '', armour: ['OBJECT_HERO_SHIRT_CHAINMAIL', 'OBJECT_IRON_LONGSWORD', 'OBJECT_NOT_IN_DATA'] });
  assert.deepEqual(r.refused, [
    { def: 'OBJECT_HERO_SHIRT_CHAINMAIL', reason: 'not_owned' },
    { def: 'OBJECT_IRON_LONGSWORD', reason: 'not_armour' },
    { def: 'OBJECT_NOT_IN_DATA', reason: 'unknown_item' },
  ]);
  // Reported but nothing accepted: the ledger's clothing no longer counts, its weapons do.
  assert.equal(r.changed, true);
  assert.deepEqual(g.loadout(1, ['OBJECT_IRON_WARHAMMER', 'OBJECT_HERO_HELMET_PLATINUM']), ['OBJECT_IRON_WARHAMMER']);
  owned.add('OBJECT_HERO_SHIRT_CHAINMAIL');
  r = g.report(1, { melee: '', ranged: '', armour: ['OBJECT_HERO_SHIRT_CHAINMAIL', 'OBJECT_HERO_SHIRT_CHAINMAIL'] });
  assert.deepEqual(r.accepted, ['OBJECT_HERO_SHIRT_CHAINMAIL']);
  assert.deepEqual(g.loadout(1, ['OBJECT_HERO_SHIRT_CHAINMAIL']), ['OBJECT_HERO_SHIRT_CHAINMAIL'], 'no repeats');
  // A report without a clothing list (unreadable) leaves the ledger's clothing in place.
  g.report(2, { melee: '', ranged: '' });
  assert.deepEqual(g.loadout(2, ['OBJECT_HERO_SHIRT_CHAINMAIL']), ['OBJECT_HERO_SHIRT_CHAINMAIL']);
  // Platinum is staff gear, trusted realm or not.
  const trusted = gear({ trust: true, staffOnly: (d) => d === 'OBJECT_HERO_HELMET_PLATINUM' }).g;
  assert.deepEqual(trusted.report(1, { melee: '', ranged: '', armour: ['OBJECT_HERO_HELMET_PLATINUM'] }).refused, [
    { def: 'OBJECT_HERO_HELMET_PLATINUM', reason: 'staff_only' },
  ]);
  assert.deepEqual(trusted.report(99, { melee: '', ranged: '', armour: ['OBJECT_HERO_HELMET_PLATINUM'] }).accepted, ['OBJECT_HERO_HELMET_PLATINUM']);
});

test('gear frame armour list is checked', () => {
  assert.deepEqual(decode('{"t":"gear","melee":"","armour":["OBJECT_HERO_SHIRT_CHAINMAIL"]}'), {
    t: 'gear',
    melee: '',
    ranged: '',
    armour: ['OBJECT_HERO_SHIRT_CHAINMAIL'],
  });
  assert.deepEqual(decode('{"t":"gear","armour":[]}'), { t: 'gear', melee: '', ranged: '', armour: [] });
  for (const bad of ['{"t":"gear","armour":"X"}', '{"t":"gear","armour":[""]}', `{"t":"gear","armour":[${Array(13).fill('"A"').join(',')}]}`])
    assert.throws(() => decode(bad), ProtocolError);
});

test('refreshLoadout swaps the weapon in and keeps health', () => {
  const app = testApp();
  const { hero } = makeHero(app);
  let equipped: string[] = [];
  const sim = new CombatSim(REGION, 'TestRegion#g', {
    data: DATA,
    spawns: { regions: {} },
    events: app.sink,
    economy: app.economy,
    progress: app.progress,
    loadout: () => ({ equipped }),
  });
  sim.addHero(hero.id, { x: 0, y: 0, z: 0 });
  const h = sim.hero(hero.id)!;
  assert.equal(h.weapon, null);
  h.health = 17;
  equipped = ['OBJECT_IRON_WARHAMMER'];
  sim.refreshLoadout(hero.id);
  assert.deepEqual(h.weapon, DATA.items.OBJECT_IRON_WARHAMMER!.weapon);
  assert.equal(h.health, 17);
  equipped = [];
  sim.refreshLoadout(hero.id);
  assert.equal(h.weapon, null);
});

// ---- over the game socket, through the whole server ----

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
function connect(url: string) {
  const ws = new WebSocket(url);
  const inbox: any[] = [];
  ws.on('message', (d) => inbox.push(JSON.parse(d.toString())));
  return { ws, inbox, open: new Promise<void>((r) => ws.on('open', () => r())) };
}
async function joinAs(app: any, base: string, name: string) {
  const post = async (p: string, body: unknown, tok?: string) =>
    (await fetch(base + p, { method: 'POST', headers: { 'content-type': 'application/json', ...(tok ? { authorization: `Bearer ${tok}` } : {}) }, body: JSON.stringify(body) })).json() as any;
  const tok = (await post('/launcher/v1/register', { username: name, password: 'password123' })).access_token;
  const hero = (await post('/launcher/v1/heroes', { name }, tok)).hero;
  app.heroes.advancePhase(hero.id, 'guild_training');
  app.heroes.advancePhase(hero.id, 'world');
  return { hero, ticket: (await post('/launcher/v1/join', { region: 'albion-1', client_version: '0.1.0' }, tok)).ticket };
}

async function swingDamage(trustClientGear: boolean, mint: boolean) {
  const app = testApp({ tickRate: 30, clientMovement: 'trusted', interestRadius: 80, pvpEverywhere: true, trustClientGear });
  const server = await listen(app, 0);
  const port = (server.address() as AddressInfo).port;
  const base = `http://127.0.0.1:${port}`;
  app.director.start();
  try {
    const one = await joinAs(app, base, 'Mobdon');
    const two = await joinAs(app, base, 'Walker');
    if (mint) app.economy.mintItem(one.hero.id, 'OBJECT_IRON_WARHAMMER', 1, 'test', key(), 'gm:1');
    const a = connect(`ws://127.0.0.1:${port}/play`);
    const b = connect(`ws://127.0.0.1:${port}/play`);
    await Promise.all([a.open, b.open]);
    a.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: one.ticket }));
    b.ws.send(JSON.stringify({ t: 'hello', protocol: 1, exe_timestamp: 'x', token: two.ticket }));
    await until(() => (a.inbox.some((m) => m.t === 'welcome') && b.inbox.some((m) => m.t === 'welcome') ? true : undefined));
    a.ws.send(JSON.stringify({ t: 'hero_pos', x: 100, y: 100, z: 0, yaw: -Math.PI / 2, area: 0 }));
    b.ws.send(JSON.stringify({ t: 'hero_pos', x: 101, y: 100, z: 0, yaw: Math.PI / 2, area: 0 }));
    const health = () => {
      for (let i = a.inbox.length - 1; i >= 0; i--) if (a.inbox[i].t === 'tick') return a.inbox[i].entities.find((e: any) => e.id === two.hero.id)?.health;
      return undefined;
    };
    let seq = 1;
    const swing = async () => {
      const before = await until(() => health());
      a.ws.send(JSON.stringify({ t: 'input', seq: seq++, buttons: 1, camera_yaw: -Math.PI / 2 }));
      await wait(100);
      a.ws.send(JSON.stringify({ t: 'input', seq: seq++, buttons: 0, camera_yaw: -Math.PI / 2 }));
      await wait(1200);
      return before - (await until(() => health()));
    };
    await wait(200);
    const bare = await swing();
    a.ws.send(JSON.stringify({ t: 'gear', melee: 'OBJECT_IRON_WARHAMMER', ranged: '' }));
    const result = await until(() => a.inbox.find((m) => m.t === 'gear_result'));
    const armed = await swing();
    a.ws.close();
    b.ws.close();
    return { bare, armed, result, events: app.sink.events.filter((e) => e.type === 'client_gear') };
  } finally {
    app.director.stop();
    server.close();
  }
}

async function armouredDamage() {
  const app = testApp({ tickRate: 30, clientMovement: 'trusted', interestRadius: 80, pvpEverywhere: true, trustClientGear: true });
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
    a.ws.send(JSON.stringify({ t: 'hero_pos', x: 100, y: 100, z: 0, yaw: -Math.PI / 2, area: 0 }));
    b.ws.send(JSON.stringify({ t: 'hero_pos', x: 101, y: 100, z: 0, yaw: Math.PI / 2, area: 0 }));
    a.ws.send(JSON.stringify({ t: 'gear', melee: 'OBJECT_IRON_WARHAMMER', ranged: '' }));
    await until(() => a.inbox.find((m) => m.t === 'gear_result'));
    const health = () => {
      for (let i = a.inbox.length - 1; i >= 0; i--) if (a.inbox[i].t === 'tick') return a.inbox[i].entities.find((e: any) => e.id === two.hero.id)?.health;
      return undefined;
    };
    let seq = 1;
    const swing = async () => {
      const before = await until(() => health());
      a.ws.send(JSON.stringify({ t: 'input', seq: seq++, buttons: 1, camera_yaw: -Math.PI / 2 }));
      await wait(100);
      a.ws.send(JSON.stringify({ t: 'input', seq: seq++, buttons: 0, camera_yaw: -Math.PI / 2 }));
      await wait(1200);
      return before - (await until(() => health()));
    };
    await wait(200);
    const bare = await swing();
    b.ws.send(JSON.stringify({ t: 'gear', melee: '', ranged: '', armour: ['OBJECT_HERO_SHIRT_CHAINMAIL'] }));
    const result = await until(() => b.inbox.find((m) => m.t === 'gear_result'));
    const armoured = await swing();
    a.ws.close();
    b.ws.close();
    return { bare, armoured, result };
  } finally {
    app.director.stop();
    server.close();
  }
}

test('over the socket: clothing worn in Fable protects the hero', async () => {
  const r = await armouredDamage();
  assert.deepEqual(r.result.accepted, ['OBJECT_HERO_SHIRT_CHAINMAIL']);
  assert.ok(r.bare > 0 && r.armoured > 0 && r.armoured < r.bare, `unarmoured took ${r.bare}, in chainmail ${r.armoured}`);
});

test('over the socket: a trusted realm fights with the weapon the mod reports', async () => {
  const r = await swingDamage(true, false);
  assert.deepEqual(r.result.accepted, ['OBJECT_IRON_WARHAMMER']);
  assert.ok(r.bare > 0, `bare-handed swing landed (${r.bare})`);
  assert.ok(r.armed > r.bare * 2, `warhammer hit ${r.armed} vs bare ${r.bare}`);
  assert.equal(r.events.length, 1);
});

test('over the socket: without trust an unowned weapon is refused, an owned one counts', async () => {
  const refused = await swingDamage(false, false);
  assert.deepEqual(refused.result.refused, [{ def: 'OBJECT_IRON_WARHAMMER', reason: 'not_owned' }]);
  assert.ok(Math.abs(refused.armed - refused.bare) < 1e-6, `still bare-handed: ${refused.bare} then ${refused.armed}`);
  const owned = await swingDamage(false, true);
  assert.deepEqual(owned.result.accepted, ['OBJECT_IRON_WARHAMMER']);
  assert.ok(owned.armed > owned.bare * 2, `owned warhammer hit ${owned.armed} vs bare ${owned.bare}`);
});
