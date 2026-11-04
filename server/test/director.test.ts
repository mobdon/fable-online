import { test } from 'node:test';
import assert from 'node:assert/strict';
import { testApp, makeHero } from './helpers.js';
import type { PlayerLink } from '../src/world/channel.js';
import { GameError } from '../src/errors.js';

const code = (c: string) => (e: unknown) => e instanceof GameError && e.code === c;

function link(heroId: number, inbox: string[] = []): PlayerLink {
  return { heroId, name: `h${heroId}`, renown: 0, lastSeq: -1, joinedAt: Date.now(), send: (f) => inbox.push(f) };
}

test('childhood and Guild training get private solo instances', () => {
  const app = testApp();
  const kid = makeHero(app, 'childhood').hero;
  const cadet = makeHero(app, 'guild_training').hero;
  const c1 = app.director.join(kid, link(kid.id));
  const c2 = app.director.join(cadet, link(cadet.id));
  assert.equal(c1.kind, 'private');
  assert.equal(c1.region.name, 'StartOakValeEast');
  assert.equal(c2.kind, 'private');
  assert.notEqual(c1, c2);
  assert.equal(app.director.stats().privateInstances, 2);
  app.director.leave(kid.id);
  assert.equal(app.director.stats().privateInstances, 1);
});

test('shared channels fill to the soft cap, then scale out, then overflow to the hard cap', () => {
  const app = testApp({ channelSoftCap: 10, channelHardCap: 12, maxChannels: 3 });
  const heroes = Array.from({ length: 36 }, () => makeHero(app).hero);
  for (const h of heroes.slice(0, 30)) app.director.join(h, link(h.id));
  let s = app.director.stats();
  assert.equal(s.sharedChannels, 3);
  assert.deepEqual(s.regions[0]!.channels.map((c) => c.players), [10, 10, 10]);
  for (const h of heroes.slice(30, 36)) app.director.join(h, link(h.id));
  s = app.director.stats();
  assert.deepEqual(s.regions[0]!.channels.map((c) => c.players), [12, 12, 12]);
  const extra = makeHero(app).hero;
  assert.throws(() => app.director.join(extra, link(extra.id)), code('region_full'));
});

test('empty shared channels close after the idle timeout', () => {
  const app = testApp({ channelIdleMs: 0 });
  const h = makeHero(app).hero;
  app.director.join(h, link(h.id));
  app.director.tick(50);
  assert.equal(app.director.stats().sharedChannels, 1);
  app.director.leave(h.id);
  app.director.tick(50);
  assert.equal(app.director.stats().sharedChannels, 0);
});

test('position is server-simulated from input and saved on leave', () => {
  const app = testApp({ heroRunSpeed: 6 }); // the distances below assume 6 m/s
  const h = makeHero(app).hero;
  const inbox: string[] = [];
  app.director.join(h, link(h.id, inbox));
  app.director.input(h.id, { seq: 1, moveX: 0, moveY: 1, buttons: 0, cameraYaw: 0 });
  for (let i = 0; i < 20; i++) app.director.tick(50); // 1 second
  app.director.input(h.id, { seq: 1, moveX: 1, moveY: 0, buttons: 0, cameraYaw: 0 }); // replayed seq: ignored
  app.director.tick(50);
  const tick = JSON.parse(inbox.at(-1)!);
  assert.equal(tick.t, 'tick');
  assert.equal(tick.last_input_seq, 1);
  assert.ok(Math.abs(tick.entities[0].y - 6.3) < 0.01, `y=${tick.entities[0].y}`);
  assert.equal(tick.entities[0].x, 0);
  app.director.leave(h.id);
  const saved = app.heroes.get(h.id)!;
  assert.ok(Math.abs(saved.y - 6.3) < 0.01);
  assert.equal(saved.region, 'HeroGuildComplex');
});

test('region transfer needs a real exit or a Cullis Gate', () => {
  const app = testApp();
  const h = makeHero(app).hero;
  app.director.join(h, link(h.id));
  assert.throws(() => app.director.transfer(h.id, 'Greatwood', { x: 0, y: 0, z: 0 }, 'exit'), code('not_adjacent'));
  assert.equal(app.director.transfer(h.id, 'LookoutPoint', { x: 1, y: 2, z: 3 }, 'exit').region.name, 'LookoutPoint');
  assert.equal(app.director.transfer(h.id, 'ArenaExterior', { x: 0, y: 0, z: 0 }, 'cullis_gate').region.name, 'ArenaExterior');
  assert.equal(app.heroes.get(h.id)!.region, 'ArenaExterior');
});

test('1,000 heroes across regions: one tick stays well under the frame budget', () => {
  const app = testApp({ maxChannels: 64 });
  const regions = ['HeroGuildComplex', 'LookoutPoint', 'BowerstoneSlums', 'Greatwood', 'ArenaExterior', 'PicnicArea'];
  for (let i = 0; i < 1000; i++) {
    const h = makeHero(app).hero;
    app.heroes.savePosition(h.id, regions[i % regions.length]!, 0, 0, 0);
    app.director.join(app.heroes.get(h.id)!, { ...link(h.id), send: () => {} });
    app.director.input(h.id, { seq: 1, moveX: Math.random() * 2 - 1, moveY: 1, buttons: 0, cameraYaw: i });
  }
  for (let i = 0; i < 20; i++) app.director.tick(50);
  const s = app.director.stats();
  assert.equal(s.players, 1000);
  assert.ok(s.sharedChannels >= 1000 / 48, `channels=${s.sharedChannels}`);
  assert.ok(s.lastTickMs < 50, `tick took ${s.lastTickMs} ms`);
  console.log(`# 1000 players, ${s.sharedChannels} channels, tick ${s.lastTickMs} ms`);
});
