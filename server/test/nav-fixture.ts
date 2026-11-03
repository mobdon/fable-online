import { NavMesh, type NavFile } from '../src/world/nav/navmesh.js';

/**
 * A synthetic level built from ASCII rows (top row = highest y): '.' walkable, '#' wall.
 * Each character is a 1 m cell, like Fable's layer-5 navigation leaves.
 */
export function asciiNav(rows: string[], heightAt: (x: number, y: number) => number = () => 10): NavMesh {
  const h = rows.length;
  const w = rows[0]!.length;
  const open = (x: number, y: number) => x >= 0 && y >= 0 && x < w && y < h && rows[h - 1 - y]![x] === '.';
  const id = (x: number, y: number) => 1 + y * w + x;
  const cells: NavFile['cells'] = [];
  for (let y = 0; y < h; y++)
    for (let x = 0; x < w; x++) {
      if (!open(x, y)) continue;
      const adj: number[] = [];
      for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]] as const) if (open(x + dx, y + dy)) adj.push(id(x + dx, y + dy));
      cells.push([id(x, y), x + 0.5, y + 0.5, 1, 0, 0, adj]);
    }
  const heights = new Float32Array((w + 1) * (h + 1));
  for (let y = 0; y <= h; y++) for (let x = 0; x <= w; x++) heights[y * (w + 1) + x] = heightAt(x, y);
  return new NavMesh({
    v: 1,
    script: 'Test',
    mapX: 0,
    mapY: 0,
    width: w,
    height: h,
    heights: Buffer.from(heights.buffer).toString('base64'),
    cells,
    exits: [],
  });
}

/** An open 40 x 40 field. */
export const field = (n = 40) => asciiNav(Array.from({ length: n }, () => '.'.repeat(n)));
