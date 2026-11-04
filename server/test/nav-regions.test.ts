import { test } from 'node:test';
import assert from 'node:assert/strict';
// @ts-expect-error plain .mjs extractor
import { mergeLevels } from '../../tools/extractor/navmesh.mjs';
import { NavMesh, type NavFile } from '../src/world/nav/navmesh.js';

/** A level doc like navmesh.mjs writes: an open w x h field of 1 m cells at height z. */
function level(script: string, w: number, h: number, z: number, wallX?: number): NavFile {
  const id = (x: number, y: number) => 1 + y * w + x;
  const open = (x: number, y: number) => x >= 0 && y >= 0 && x < w && y < h && x !== wallX;
  const cells: NavFile['cells'] = [];
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      if (!open(x, y)) continue;
      const adj: number[] = [];
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) if (open(x + dx, y + dy)) adj.push(id(x + dx, y + dy));
      cells.push([id(x, y), x + 0.5, y + 0.5, 1, 0, 0, adj]);
    }
  const heights = new Float32Array((w + 1) * (h + 1)).fill(z);
  return { v: 1, script, mapX: 0, mapY: 0, width: w, height: h, heights: Buffer.from(heights.buffer).toString('base64'), cells, exits: [[id(0, 0), ['123']]] };
}

test('a region joins its levels in the anchor frame and links cells across the seam', () => {
  // Anchor A at 0,0; B sits west of it (negative offset), so the region origin is negative.
  const merged = mergeLevels('R', [
    { doc: level('A', 10, 6, 5), dx: 0, dy: 0 },
    { doc: level('B', 8, 6, 7), dx: -8, dy: 0 },
  ]) as NavFile & { seams: number };
  assert.equal(merged.originX, -8);
  assert.equal(merged.width, 18);
  assert.equal(merged.seams, 6, 'one link per 1 m row along the shared edge');
  const nav = new NavMesh(merged);
  assert.ok(nav.walkable(-4, 3), 'B is walkable at its region position');
  assert.ok(nav.walkable(4, 3), 'A is walkable at its own position');
  assert.ok(!nav.walkable(-9, 3), 'nothing west of B');
  assert.equal(nav.heightAt(-4, 3), 7);
  assert.equal(nav.heightAt(4, 3), 5);
  const path = nav.findPath({ x: -6, y: 3 }, { x: 8, y: 3 });
  assert.ok(path, 'a creature can walk from B into A');
  assert.equal(merged.exits.length, 2, 'exit cells kept, with new ids');
});

test('walls inside a level still block, and the path goes round them', () => {
  // A wall along x = 5 with a gap only at the top row: the path has to climb to it.
  const doc = level('A', 10, 6, 0, 5);
  doc.cells.push([999, 5.5, 5.5, 1, 0, 0, [1 + 5 * 10 + 4, 1 + 5 * 10 + 6]]);
  for (const c of doc.cells) if (c[0] === 1 + 5 * 10 + 4 || c[0] === 1 + 5 * 10 + 6) c[6].push(999);
  const nav = new NavMesh(mergeLevels('R', [{ doc, dx: 0, dy: 0 }]));
  assert.ok(!nav.walkable(5.5, 1.5));
  assert.ok(!nav.clearLine({ x: 2, y: 1.5 }, { x: 8, y: 1.5 }));
  const path = nav.findPath({ x: 2, y: 1.5 }, { x: 8, y: 1.5 })!;
  assert.ok(path.some((p) => p.y > 5), 'went round through the gap');
});
