import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createApp } from '../src/app.js';
import { MemorySink } from '../src/analytics/events.js';
import type { PlayerLink } from '../src/world/channel.js';
import { WorldThings, type GateThing, type WorldThingsFile } from '../src/world/things/world-things.js';
import { StubSim, type SimFactory } from '../src/world/sim.js';
import { decode, ProtocolError } from '../src/net/protocol.js';
import { gateKind } from '../../tools/extractor/things.mjs';
import { makeHero } from './helpers.js';

const gate = (id: string, pos: [number, number, number], o: Partial<GateThing> = {}): GateThing => {
  const [region, uid] = id.split(':') as [string, string];
  return { id, uid, def: 'OBJECT_TELEPORTER_GRIFFONS_01', level: region, region, section: 'NULL', script: null, pos, fwd: null, guild: false, ...o };
};

function fixture(): WorldThingsFile {
  return {
    v: 1,
    exits: [],
    doors: [],
    chests: [],
    containers: [],
    pickups: [],
    digSpots: [],
    demonDoors: [],
    hooks: [],
    gates: [
      gate('HeroGuildComplex:1', [0, 30, 0], { def: 'OBJECT_GUILD_PEDESTAL_TELEPORT_01', guild: true }),
      gate('LookoutPoint:5', [50, 50, 0], { fwd: [0, 1] }),
      gate('Greatwood:7', [100, 100, 0]),
      gate('PrisonCourtyard:2', [10, 10, 0]),
    ],
  };
}

/** Heroes the test has put "in a fight": the stub sim reports their last hit as now. */
const fighting = new Set<number>();
const down = new Set<number>();
const sim: SimFactory = () => {
  const s = new StubSim() as StubSim & { combatState(id: number): { alive: boolean; lastHitAt: number } | undefined };
  s.combatState = (id) => (s.position(id) ? { alive: !down.has(id), lastHitAt: fighting.has(id) ? Date.now() : 0 } : undefined);
  return s;
};

function setup() {
  const sink = new MemorySink();
  const app = createApp({ dbPath: ':memory:', eventsPath: '', regionsPath: '/nonexistent', thingsPath: '' }, { events: sink, things: new WorldThings(fixture()), sim });
  return Object.assign(app, { sink });
}
type TestApp = ReturnType<typeof setup>;

function join(app: TestApp, region = 'HeroGuildComplex', at: [number, number, number] = [0, 0, 0], phase: 'world' | 'guild_training' = 'world') {
  const { hero } = makeHero(app, phase);
  app.heroes.savePosition(hero.id, region, ...at);
  const inbox: any[] = [];
  const l: PlayerLink = { heroId: hero.id, name: hero.name, renown: 0, lastSeq: -1, joinedAt: Date.now(), send: (f) => inbox.push(JSON.parse(f)) };
  app.director.join(app.heroes.get(hero.id)!, l);
  return { id: hero.id, inbox };
}

/** Let the scan run again and step the world once. */
const tick = (app: TestApp) => {
  (app.travel as any).lastScan = 0;
  app.director.tick(50);
};
const arrived = (app: TestApp, id: number) => app.director.regionReady(id, app.director.channelOf(id)!.region.name);
const regionOf = (app: TestApp, id: number) => app.director.channelOf(id)?.region.name;

test('a gate unlocks when the server has the hero standing near it, once, and stays unlocked', () => {
  const app = setup();
  const near = join(app, 'LookoutPoint', [52, 48, 0]);
  const far = join(app, 'LookoutPoint', [200, 200, 0]);
  tick(app);
  assert.ok(near.inbox.some((m) => m.t === 'gate_unlocked' && m.id === 'LookoutPoint:5'));
  assert.ok(!far.inbox.some((m) => m.t === 'gate_unlocked'));
  tick(app);
  assert.equal(near.inbox.filter((m) => m.t === 'gate_unlocked').length, 1);
  assert.equal(app.sink.events.filter((e) => e.type === 'gate_unlocked').length, 1);
  // Kept in the database: still there after logging out and back in.
  app.director.leave(near.id);
  assert.ok(app.travel.unlockedOf(near.id).has('LookoutPoint:5'));
  const list = app.travel.list(far.id).gates as { id: string; guild: boolean }[];
  assert.deepEqual(list.map((g) => g.id), ['HeroGuildComplex:1']);
  app.close();
});

test('a hero can only travel to gates they have found; the Guild is always open', () => {
  fighting.clear();
  const app = setup();
  const a = join(app, 'LookoutPoint', [52, 48, 0]);
  assert.equal(app.travel.travel(a.id, { gate: 'Greatwood:7' }).error, 'locked');
  assert.equal(app.travel.travel(a.id, { gate: 'Nowhere:1' }).error, 'no_gate');
  // A test server can open every gate (FO_GATES_OPEN).
  app.travel.rules.allOpen = true;
  assert.equal((app.travel.list(a.id).gates as unknown[]).length, 4);
  app.travel.rules.allOpen = false;
  tick(app); // unlocks Lookout Point
  const r = app.travel.travel(a.id, { guild: true });
  assert.equal(r.ok, true);
  assert.equal(regionOf(app, a.id), 'HeroGuildComplex');
  const enter = a.inbox.findLast((m) => m.t === 'enter_region');
  assert.equal(enter.via, 'cullis_gate');
  assert.equal(enter.thing, 'HeroGuildComplex:1');
  assert.equal(enter.loading, true);
  // Back to Lookout Point by its gate: arrives a step out of the gate along its facing.
  arrived(app, a.id);
  app.travel.rules.cooldownMs = 0;
  assert.equal(app.travel.travel(a.id, { gate: 'LookoutPoint:5' }).ok, true);
  const pos = app.director.channelOf(a.id)!.sim.position(a.id)!;
  assert.equal(regionOf(app, a.id), 'LookoutPoint');
  assert.deepEqual([pos.x, pos.y], [50, 52.5]);
  app.close();
});

test('a Guild Seal jump remembers the way back, once', () => {
  const app = setup();
  app.travel.rules.cooldownMs = 0;
  const a = join(app, 'Greatwood', [300, 300, 0]);
  assert.equal(app.travel.travel(a.id, { back: true }).error, 'no_return');
  assert.equal(app.travel.travel(a.id, { guild: true }).ok, true);
  assert.deepEqual(app.travel.list(a.id).back, { region: 'Greatwood' });
  arrived(app, a.id);
  assert.equal(app.travel.travel(a.id, { back: true }).ok, true);
  assert.equal(regionOf(app, a.id), 'Greatwood');
  assert.deepEqual(app.director.channelOf(a.id)!.sim.position(a.id), { x: 300, y: 300, z: 0 });
  arrived(app, a.id);
  assert.equal(app.travel.travel(a.id, { back: true }).error, 'no_return');
  app.close();
});

test('gate-to-gate travel needs the hero in the gate, and does not set a way back', () => {
  const app = setup();
  app.travel.rules.cooldownMs = 0;
  const a = join(app, 'LookoutPoint', [80, 80, 0]);
  assert.equal(app.travel.travel(a.id, { guild: true }, 'LookoutPoint:5').error, 'too_far');
  assert.equal(app.travel.travel(a.id, { guild: true }, 'Greatwood:7').error, 'too_far');
  app.director.channelOf(a.id)!.sim.placeHero!(a.id, { x: 51, y: 50, z: 0 });
  assert.equal(app.travel.travel(a.id, { guild: true }, 'LookoutPoint:5').ok, true);
  // Standing in it unlocked it on the spot.
  assert.ok(app.travel.unlockedOf(a.id).has('LookoutPoint:5'));
  assert.equal(app.travel.list(a.id).back, null);
  app.close();
});

test('walking into a gate unlocks it and answers with the travel list', () => {
  const app = setup();
  const a = join(app, 'LookoutPoint', [50, 49, 0]);
  const far = join(app, 'LookoutPoint', [90, 90, 0]);
  const r = app.travel.useGate(a.id, 'LookoutPoint:5');
  assert.equal(r.t, 'gates');
  assert.deepEqual((r.gates as { id: string }[]).map((g) => g.id).sort(), ['HeroGuildComplex:1', 'LookoutPoint:5']);
  assert.equal(app.travel.useGate(far.id, 'LookoutPoint:5').error, 'too_far');
  assert.equal(app.travel.useGate(a.id, 'Greatwood:7').error, 'not_here');
  app.close();
});

test('no travel in a fight, while down, during the cooldown, from barred regions or before graduating', () => {
  fighting.clear();
  down.clear();
  const app = setup();
  const a = join(app, 'Greatwood', [300, 300, 0]);
  fighting.add(a.id);
  assert.equal(app.travel.travel(a.id, { guild: true }).error, 'in_combat');
  fighting.delete(a.id);
  down.add(a.id);
  assert.equal(app.travel.travel(a.id, { guild: true }).error, 'dead');
  down.delete(a.id);
  assert.equal(app.travel.travel(a.id, { guild: true }).ok, true);
  arrived(app, a.id);
  assert.equal(app.travel.travel(a.id, { back: true }).error, 'cooldown');

  const jailed = join(app, 'PrisonCourtyard', [10, 10, 0]);
  assert.equal(app.travel.travel(jailed.id, { guild: true }).error, 'barred');

  const pupil = join(app, 'HeroGuildComplex', [0, 0, 0], 'guild_training');
  assert.equal(app.travel.travel(pupil.id, { guild: true }).error, 'not_graduated');

  assert.ok(app.sink.events.some((e) => e.type === 'gate_travel' && e.refused === 'in_combat'));
  app.close();
});

test('nearby party members travel along; far or fighting ones stay behind and are told', () => {
  fighting.clear();
  const app = setup();
  const lead = join(app, 'Greatwood', [300, 300, 0]);
  const near = join(app, 'Greatwood', [305, 300, 0]);
  const far = join(app, 'Greatwood', [400, 400, 0]);
  const busy = join(app, 'Greatwood', [300, 305, 0]);
  const party = [lead.id, near.id, far.id, busy.id];
  app.travel.parties = { members: (id) => (party.includes(id) ? party : []) };
  fighting.add(busy.id);
  const r = app.travel.travel(lead.id, { guild: true });
  assert.equal(r.ok, true);
  assert.deepEqual(r.party, [near.id]);
  assert.equal(regionOf(app, near.id), 'HeroGuildComplex');
  assert.equal(regionOf(app, far.id), 'Greatwood');
  assert.equal(regionOf(app, busy.id), 'Greatwood');
  assert.equal(near.inbox.findLast((m) => m.t === 'travel_result').with, lead.id);
  assert.equal(busy.inbox.findLast((m) => m.t === 'travel_result').error, 'in_combat');
  assert.ok(!far.inbox.some((m) => m.t === 'travel_result'));
  // They land next to each other, not on top of each other.
  const p1 = app.director.channelOf(lead.id)!.sim.position(lead.id)!;
  const p2 = app.director.channelOf(near.id)!.sim.position(near.id)!;
  assert.ok(Math.hypot(p1.x - p2.x, p1.y - p2.y) > 1);
  // Members who went along can go back to where they were too.
  assert.deepEqual(app.travel.list(near.id).back, { region: 'Greatwood' });
  fighting.clear();
  app.close();
});

test('travel requests on the wire', () => {
  assert.deepEqual(decode('{"t":"travel","to":"guild"}'), { t: 'travel', to: 'guild' });
  assert.deepEqual(decode('{"t":"travel","to":"back"}'), { t: 'travel', to: 'back' });
  assert.deepEqual(decode('{"t":"travel","to":"Greatwood:7","from":"LookoutPoint:5"}'), { t: 'travel', to: 'Greatwood:7', from: 'LookoutPoint:5' });
  assert.deepEqual(decode('{"t":"gates"}'), { t: 'gates' });
  assert.throws(() => decode('{"t":"travel","to":"x; drop"}'), ProtocolError);
  assert.throws(() => decode('{"t":"travel","to":"guild","from":5}'), ProtocolError);
});

test('the extractor picks Cullis Gates by component, not house teleporters or switched-off models', () => {
  const t = (def: string, tc: Record<string, object> = {}) => ({ def, tc });
  assert.equal(gateKind(t('OBJECT_ANY', { CTCTeleporter: {} })), 'gate');
  assert.equal(gateKind(t('OBJECT_TELEPORTER_GRIFFONS_01')), 'gate');
  assert.equal(gateKind(t('OBJECT_GUILD_PEDESTAL_TELEPORT_01', { CTCTeleporter: {} })), 'guild');
  assert.equal(gateKind(t('OBJECT_TELEPORTER_BUYABLE_HOUSE_01', { CTCTeleporter: {} })), null);
  assert.equal(gateKind(t('OBJECT_HKC_TELEPORTER_OFF_01', { CTCTeleporter: {} })), null);
  assert.equal(gateKind(t('OBJECT_BARREL')), null);
});
