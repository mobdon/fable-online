import { test } from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { NavStore, seededRng } from '../src/world/nav/navmesh.js';
import { asciiNav } from './nav-fixture.js';

//            x: 0123456789
const ROOM = [
  '..........', // y = 9
  '..........',
  '.########.',
  '.#......#.',
  '.#......#.',
  '.#......#.',
  '.####.###.', // y = 3: a one-metre door at x = 5
  '..........',
  '..........',
  '..........', // y = 0
];

test('walkable, cellAt and heightAt follow the level data', () => {
  const nav = asciiNav(ROOM, (x, y) => x + 2 * y);
  assert.equal(nav.walkable(0.5, 0.5), true);
  assert.equal(nav.walkable(1.5, 4.5), false); // wall
  assert.equal(nav.walkable(-1, 0), false);
  assert.equal(nav.walkable(5.5, 3.5), true); // door
  assert.ok(Math.abs(nav.heightAt(2.5, 1.25) - (2.5 + 2.5)) < 1e-4); // bilinear on a plane is exact
});

test('findPath goes round walls through the door and stays on the ground', () => {
  const nav = asciiNav(ROOM);
  const inside = { x: 4.5, y: 5.5 };
  const outside = { x: 0.5, y: 0.5 };
  const p = nav.findPath(outside, inside)!;
  assert.ok(p, 'there is a way in');
  const last = p[p.length - 1]!;
  assert.deepEqual(last, inside);
  // Every leg must be walkable end to end, and the route has to pass the door.
  let at = outside;
  for (const wp of p) {
    assert.ok(nav.clearLine(at, wp), `leg ${at.x},${at.y} -> ${wp.x},${wp.y} crosses a wall`);
    at = wp;
  }
  assert.ok(p.some((wp) => Math.abs(wp.x - 5.5) < 0.6 && wp.y >= 2.9 && wp.y <= 4.1), 'goes through the door');
  // String pulling: a handful of waypoints, not one per cell.
  assert.ok(p.length <= 4, `path has ${p.length} points`);
});

test('a sealed room has no path, and the island check answers without searching', () => {
  const sealed = ROOM.map((r, i) => (i === 6 ? '.########.' : r));
  const nav = asciiNav(sealed);
  assert.equal(nav.findPath({ x: 0.5, y: 0.5 }, { x: 4.5, y: 5.5 }), undefined);
});

test('snap moves an off-mesh point onto the nearest ground', () => {
  const nav = asciiNav(ROOM);
  const s = nav.snap({ x: 1.5, y: 4.5 })!; // inside a wall
  assert.ok(nav.walkable(s.x, s.y));
  assert.ok(Math.hypot(s.x - 1.5, s.y - 4.5) < 1.2);
});

test('randomPointNear only returns reachable ground', () => {
  const nav = asciiNav(ROOM.map((r, i) => (i === 6 ? '.########.' : r)));
  const rng = seededRng(7);
  for (let i = 0; i < 50; i++) {
    const p = nav.randomPointNear({ x: 0.5, y: 0.5 }, 9, rng);
    if (!p) continue;
    assert.ok(nav.walkable(p.x, p.y));
    assert.ok(!(p.x > 2 && p.x < 8 && p.y > 3 && p.y < 7), 'never inside the sealed room');
  }
});

// Runs only when the navmesh has been extracted from a real install (not in CI).
const REAL = path.resolve(import.meta.dirname, '../../shared/defs/nav');
test('real Fable data: Lookout Point loads, matches the .tng heights, and paths work', { skip: !fs.existsSync(path.join(REAL, 'LookoutPoint.json')) }, () => {
  const nav = new NavStore(REAL).get('LookoutPoint')!;
  assert.equal(nav.width, 128);
  // A marker the game places at (49.67, 76.65, 35.25) in LookoutPoint.tng.
  assert.ok(Math.abs(nav.heightAt(49.669, 76.648) - 35.25) < 0.3);
  assert.ok(nav.walkable(49.669, 76.648));
  assert.ok(nav.exits().length > 0);
  const rng = seededRng(3);
  for (let i = 0; i < 100; i++) {
    const a = nav.cells[Math.floor(rng() * nav.cells.length)]!;
    const b = nav.cells[Math.floor(rng() * nav.cells.length)]!;
    if (!nav.connected(a, b)) continue;
    const p = nav.findPath(a, b);
    assert.ok(p, `no path ${a.x},${a.y} -> ${b.x},${b.y}`);
    let at: { x: number; y: number } = a;
    for (const wp of p) {
      assert.ok(nav.clearLine(at, wp));
      at = wp;
    }
  }
});
