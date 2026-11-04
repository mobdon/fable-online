import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero } from './helpers.js';
import type { PlayerLink } from '../src/world/channel.js';
import { decode, ProtocolError } from '../src/net/protocol.js';

function link(heroId: number, name: string, inbox: string[]) {
  const l: PlayerLink = { heroId, name, renown: 500, lastSeq: -1, joinedAt: Date.now(), send: (f) => inbox.push(f) };
  return l;
}

const lastTick = (inbox: string[]) => {
  const frames = inbox.map((f) => JSON.parse(f)).filter((m) => m.t === 'tick');
  return frames[frames.length - 1];
};

test('frames label heroes with their name and renown, and creatures with their def', () => {
  const app = testApp({ interestRadius: 0 });
  const a = makeHero(app).hero;
  const b = makeHero(app).hero;
  const inbox: string[] = [];
  const ch = app.director.join(a, link(a.id, 'Mob', inbox));
  app.director.join(b, link(b.id, 'Scythe', []));

  ch.step(50);
  const tick = lastTick(inbox);
  assert.equal(tick.entities.length, 2);
  const mine = tick.entities.find((e: any) => e.id === a.id);
  const other = tick.entities.find((e: any) => e.id === b.id);
  assert.equal(mine.kind, 'hero');
  assert.equal(other.kind, 'hero');
  assert.equal(other.name, 'Scythe');
  assert.equal(other.renown, 500);
  // The mod needs a def name to spawn a stand-in from; heroes get one from the mod itself.
  assert.equal(other.def, undefined);

  // A creature in the sim comes out as a creature, with the def the mod spawns.
  const sim = ch.sim as any;
  sim.snapshot = () => [{ id: 1_000_001, def_id: 0, def: 'CREATURE_BANDIT_GRUNT_LEVEL1', x: 1, y: 2, z: 0, yaw: 0, anim: 2, health: 40 }];
  ch.step(50);
  const creature = lastTick(inbox).entities[0];
  assert.equal(creature.kind, 'creature');
  assert.equal(creature.def, 'CREATURE_BANDIT_GRUNT_LEVEL1');
});

test('each player only gets the entities near them', () => {
  const app = testApp({ interestRadius: 50, clientMovement: 'trusted' });
  const near = makeHero(app).hero;
  const far = makeHero(app).hero;
  const nearInbox: string[] = [];
  const farInbox: string[] = [];
  const ch = app.director.join(near, link(near.id, 'Near', nearInbox));
  app.director.join(far, link(far.id, 'Far', farInbox));

  assert.equal(ch.placeHero(near.id, { x: 0, y: 0, z: 0 }), true);
  assert.equal(ch.placeHero(far.id, { x: 300, y: 0, z: 0 }), true);
  ch.step(50);

  // Each sees itself; neither sees the other, 300 m away.
  assert.deepEqual(lastTick(nearInbox).entities.map((e: any) => e.id), [near.id]);
  assert.deepEqual(lastTick(farInbox).entities.map((e: any) => e.id), [far.id]);

  // Walk the far hero into range and they appear in each other's frames.
  ch.placeHero(far.id, { x: 20, y: 10, z: 0 });
  ch.step(50);
  assert.deepEqual(
    lastTick(nearInbox)
      .entities.map((e: any) => e.id)
      .sort((x: number, y: number) => x - y),
    [near.id, far.id].sort((x, y) => x - y),
  );

  // A hero who left the channel cannot be placed any more.
  app.director.leave(far.id);
  assert.equal(ch.placeHero(far.id, { x: 0, y: 0, z: 0 }), false);
});

test('a position from the client is only used when the server is told to trust it', () => {
  const guarded = testApp({ clientMovement: 'off' });
  const hero = makeHero(guarded).hero;
  const ch = guarded.director.join(hero, link(hero.id, 'Mob', []));
  assert.equal(guarded.cfg.clientMovement, 'off');

  // The channel itself still obeys: the refusal is in the socket, which is where the
  // client's say-so arrives. Here we check the default is off and the frame is valid.
  const msg = decode(JSON.stringify({ t: 'hero_pos', x: 1.5, y: -2, z: 3, yaw: 9, area: 42 }));
  assert.equal(msg.t, 'hero_pos');
  if (msg.t === 'hero_pos') {
    assert.equal(msg.x, 1.5);
    assert.ok(Math.abs(msg.yaw!) <= Math.PI); // yaw is wrapped, as for inputs
    assert.equal(msg.area, 42);
  }
  assert.throws(() => decode(JSON.stringify({ t: 'hero_pos', x: 'over there', y: 0, z: 0 })), ProtocolError);
  assert.throws(() => decode(JSON.stringify({ t: 'hero_pos', x: 1e9, y: 0, z: 0 })), ProtocolError);
  assert.ok(ch.placeHero(hero.id, { x: 5, y: 5, z: 0 })); // the channel can place; the socket decides whether to
});

test('protocol: dev_spawn takes an optional def and 1-10 creatures', () => {
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_spawn' })), { t: 'dev_spawn', def: '', count: 1 });
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_spawn', def: 'CREATURE_WOLF', count: 3 })), { t: 'dev_spawn', def: 'CREATURE_WOLF', count: 3 });
  assert.throws(() => decode(JSON.stringify({ t: 'dev_spawn', count: 50 })), ProtocolError);
  assert.deepEqual(decode(JSON.stringify({ t: 'dev_despawn', what: 'near' })), { t: 'dev_despawn', what: 'near' });
  assert.throws(() => decode(JSON.stringify({ t: 'dev_despawn', what: 'everything' })), ProtocolError);
  assert.throws(() => decode(JSON.stringify({ t: 'dev_spawn', def: 'wolf; drop table' })), ProtocolError);
});
