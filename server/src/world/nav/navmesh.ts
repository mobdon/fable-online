import fs from 'node:fs';
import path from 'node:path';
import type { Vec3 } from '../sim.js';

/**
 * Walkable ground for one region, read from shared/defs/nav/<Script>.json, which
 * tools/extractor/navmesh.mjs builds from the player's own FinalAlbion.wad.
 *
 * Coordinates are the level's local frame, the same one the .tng files use: metres,
 * x/y across the ground, z up, origin at the level's corner. The level sits at
 * (mapX, mapY) in Albion.
 *
 * The ground is a set of axis-aligned square cells (Fable's navigation quadtree leaves,
 * 32 m down to 0.5 m). Each cell knows which cells it touches, so the cells are the path
 * graph. Anything not covered by a cell is a wall, a cliff, water or scenery.
 */
export interface NavFile {
  v: 1;
  script: string;
  mapX: number;
  mapY: number;
  /**
   * Region files (nav-regions/<Region>.json) join several levels in the region's frame; the
   * heightmap's corner then sits here rather than at 0,0. Level files leave these out.
   */
  originX?: number;
  originY?: number;
  width: number;
  height: number;
  /** base64 Float32Array of (width+1)*(height+1) ground heights, row-major, 1 m apart. */
  heights: string;
  /** [id, centreX, centreY, size, subset, zLevel, adjacentIds] */
  cells: [number, number, number, number, number, number, number[]][];
  /** [cellId, uids of the region-exit things beside it] */
  exits: [number, string[]][];
}

export interface Cell {
  /** Index into NavMesh.cells (not Fable's node id). */
  index: number;
  x: number;
  y: number;
  half: number;
  subset: number;
  adj: number[];
  exitUids?: string[];
}

export interface Point2 {
  x: number;
  y: number;
}

/** A tiny seeded PRNG so AI behaviour is reproducible in tests. */
export type Rng = () => number;

export class NavMesh {
  readonly script: string;
  readonly mapX: number;
  readonly mapY: number;
  readonly width: number;
  readonly height: number;
  readonly originX: number;
  readonly originY: number;
  readonly cells: Cell[];
  private readonly heights: Float32Array;
  /** 1 m buckets -> indices of cells overlapping that square. */
  private readonly grid: Int32Array[];
  /** Connected component per cell, so "can I get there at all" is O(1). */
  private readonly island: Int32Array;

  constructor(f: NavFile) {
    this.script = f.script;
    this.mapX = f.mapX;
    this.mapY = f.mapY;
    this.width = f.width;
    this.height = f.height;
    this.originX = f.originX ?? 0;
    this.originY = f.originY ?? 0;
    const ox = this.originX;
    const oy = this.originY;
    const raw = Buffer.from(f.heights, 'base64');
    this.heights = new Float32Array(raw.buffer.slice(raw.byteOffset, raw.byteOffset + raw.byteLength));
    if (this.heights.length !== (f.width + 1) * (f.height + 1)) throw new Error(`${f.script}: heightmap size mismatch`);

    const byId = new Map<number, number>();
    f.cells.forEach((c, i) => byId.set(c[0], i));
    this.cells = f.cells.map((c, i) => ({
      index: i,
      x: c[1],
      y: c[2],
      half: c[3] / 2,
      subset: c[4],
      adj: c[6].map((id) => byId.get(id)).filter((n): n is number => n !== undefined),
    }));
    for (const [id, uids] of f.exits) {
      const i = byId.get(id);
      if (i !== undefined) this.cells[i]!.exitUids = uids;
    }

    const buckets: number[][] = Array.from({ length: f.width * f.height }, () => []);
    for (const c of this.cells) {
      const x0 = Math.max(0, Math.floor(c.x - c.half - ox));
      const x1 = Math.min(f.width - 1, Math.ceil(c.x + c.half - ox) - 1);
      const y0 = Math.max(0, Math.floor(c.y - c.half - oy));
      const y1 = Math.min(f.height - 1, Math.ceil(c.y + c.half - oy) - 1);
      for (let y = y0; y <= y1; y++) for (let x = x0; x <= x1; x++) buckets[y * f.width + x]!.push(c.index);
    }
    this.grid = buckets.map((b) => Int32Array.from(b));

    this.island = new Int32Array(this.cells.length).fill(-1);
    let island = 0;
    for (const start of this.cells) {
      if (this.island[start.index] !== -1) continue;
      const stack = [start.index];
      this.island[start.index] = island;
      while (stack.length) {
        const i = stack.pop()!;
        for (const j of this.cells[i]!.adj) {
          if (this.island[j] === -1) {
            this.island[j] = island;
            stack.push(j);
          }
        }
      }
      island++;
    }
  }

  static load(file: string): NavMesh {
    return new NavMesh(JSON.parse(fs.readFileSync(file, 'utf8')) as NavFile);
  }

  /** The walkable cell under (x, y), or undefined over a wall, cliff or water. */
  cellAt(x: number, y: number): Cell | undefined {
    const gx = x - this.originX;
    const gy = y - this.originY;
    if (!(gx >= 0 && gy >= 0 && gx < this.width && gy < this.height)) return undefined;
    const b = this.grid[Math.floor(gy) * this.width + Math.floor(gx)]!;
    for (const i of b) {
      const c = this.cells[i]!;
      if (Math.abs(x - c.x) <= c.half && Math.abs(y - c.y) <= c.half) return c;
    }
    return undefined;
  }

  walkable(x: number, y: number): boolean {
    return this.cellAt(x, y) !== undefined;
  }

  /** Ground height in metres, bilinear between the 1 m heightmap samples. */
  heightAt(x: number, y: number): number {
    x -= this.originX;
    y -= this.originY;
    const w = this.width;
    const fx = Math.min(Math.max(x, 0), w);
    const fy = Math.min(Math.max(y, 0), this.height);
    const x0 = Math.min(Math.floor(fx), w - 1);
    const y0 = Math.min(Math.floor(fy), this.height - 1);
    const tx = fx - x0;
    const ty = fy - y0;
    const h = this.heights;
    const row = w + 1;
    const a = h[y0 * row + x0]!;
    const b = h[y0 * row + x0 + 1]!;
    const c = h[(y0 + 1) * row + x0]!;
    const d = h[(y0 + 1) * row + x0 + 1]!;
    return a + (b - a) * tx + (c - a) * ty + (a - b - c + d) * tx * ty;
  }

  /** Nearest walkable cell to a point, searching outward up to maxDist metres. */
  nearestCell(x: number, y: number, maxDist = 8): Cell | undefined {
    const here = this.cellAt(x, y);
    if (here) return here;
    let best: Cell | undefined;
    let bestD = maxDist * maxDist;
    const r = Math.ceil(maxDist);
    const gx = Math.floor(x - this.originX);
    const gy = Math.floor(y - this.originY);
    for (let by = Math.max(0, gy - r); by <= Math.min(this.height - 1, gy + r); by++) {
      for (let bx = Math.max(0, gx - r); bx <= Math.min(this.width - 1, gx + r); bx++) {
        for (const i of this.grid[by * this.width + bx]!) {
          const c = this.cells[i]!;
          const px = clamp(x, c.x - c.half, c.x + c.half);
          const py = clamp(y, c.y - c.half, c.y + c.half);
          const d = (px - x) ** 2 + (py - y) ** 2;
          if (d < bestD) {
            bestD = d;
            best = c;
          }
        }
      }
    }
    return best;
  }

  /** Snap a point onto walkable ground (for spawns and knockback). */
  snap(p: Point2, maxDist = 8): Vec3 | undefined {
    const c = this.nearestCell(p.x, p.y, maxDist);
    if (!c) return undefined;
    const m = c.half - 0.05;
    const x = clamp(p.x, c.x - m, c.x + m);
    const y = clamp(p.y, c.y - m, c.y + m);
    return { x, y, z: this.heightAt(x, y) };
  }

  connected(a: Cell, b: Cell): boolean {
    return this.island[a.index] === this.island[b.index];
  }

  /**
   * True if a straight walk from a to b stays on walkable ground. Samples every 0.25 m,
   * half the smallest cell, so it can't step over a gap.
   */
  clearLine(a: Point2, b: Point2): boolean {
    const dx = b.x - a.x;
    const dy = b.y - a.y;
    const steps = Math.max(1, Math.ceil(Math.hypot(dx, dy) / 0.25));
    for (let i = 0; i <= steps; i++) {
      const t = i / steps;
      if (!this.walkable(a.x + dx * t, a.y + dy * t)) return false;
    }
    return true;
  }

  /**
   * Shortest walk from a to b: A* over the cells, then pulled tight so creatures run
   * straight where the ground allows instead of zig-zagging cell centre to cell centre.
   * Returns the waypoints after `a` (ending exactly at b, or at the closest reachable
   * point if b is off the mesh), or undefined if there is no way there.
   */
  findPath(a: Point2, b: Point2, maxExpanded = 4000): Point2[] | undefined {
    const start = this.nearestCell(a.x, a.y, 2);
    let goal = this.nearestCell(b.x, b.y, 4);
    if (!start || !goal) return undefined;
    if (!this.connected(start, goal)) return undefined;
    let end: Point2 = this.cellAt(b.x, b.y) ? b : this.snap(b, 4)!;
    if (start === goal || this.clearLine(a, end)) return [end];

    const n = this.cells.length;
    const g = new Float64Array(n).fill(Infinity);
    const came = new Int32Array(n).fill(-1);
    const closed = new Uint8Array(n);
    const open = new MinHeap();
    g[start.index] = 0;
    open.push(start.index, dist(start, goal));
    let expanded = 0;
    let found = false;
    while (open.size) {
      const i = open.pop();
      if (closed[i]) continue;
      if (i === goal.index) {
        found = true;
        break;
      }
      closed[i] = 1;
      if (++expanded > maxExpanded) break;
      const c = this.cells[i]!;
      for (const j of c.adj) {
        if (closed[j]) continue;
        const nb = this.cells[j]!;
        const cost = g[i]! + dist(c, nb);
        if (cost < g[j]!) {
          g[j] = cost;
          came[j] = i;
          open.push(j, cost + dist(nb, goal));
        }
      }
    }
    if (!found) return undefined;

    // Portal points: the middle of the shared edge between consecutive cells. Walking
    // through edge midpoints keeps the path inside the mesh even between unequal cells.
    const chain: number[] = [];
    for (let i = goal.index; i !== -1; i = came[i]!) chain.push(i);
    chain.reverse();
    const pts: Point2[] = [];
    for (let k = 1; k < chain.length; k++) pts.push(portal(this.cells[chain[k - 1]!]!, this.cells[chain[k]!]!));
    pts.push(end);

    // String pulling: from each anchor, jump to the farthest point still in clear sight.
    const out: Point2[] = [];
    let anchor: Point2 = a;
    let k = 0;
    while (k < pts.length) {
      let far = k;
      for (let j = pts.length - 1; j > k; j--) {
        if (this.clearLine(anchor, pts[j]!)) {
          far = j;
          break;
        }
      }
      out.push(pts[far]!);
      anchor = pts[far]!;
      k = far + 1;
    }
    return out;
  }

  /** A random walkable point within radius r of p that can be walked to from p. */
  randomPointNear(p: Point2, r: number, rng: Rng, tries = 12): Point2 | undefined {
    const from = this.nearestCell(p.x, p.y, 2);
    if (!from) return undefined;
    for (let t = 0; t < tries; t++) {
      const ang = rng() * Math.PI * 2;
      const d = Math.sqrt(rng()) * r;
      const x = p.x + Math.cos(ang) * d;
      const y = p.y + Math.sin(ang) * d;
      const c = this.cellAt(x, y);
      if (c && this.connected(from, c)) return { x, y };
    }
    return undefined;
  }

  /** Cells that lead out of this region, with the UIDs of the exit things in the .tng. */
  exits(): Cell[] {
    return this.cells.filter((c) => c.exitUids);
  }
}

/** Loads each region's navmesh once, on first use. Regions with no file get undefined. */
export class NavStore {
  private cache = new Map<string, NavMesh | null>();
  constructor(private dir: string) {}

  get(script: string): NavMesh | undefined {
    let m = this.cache.get(script);
    if (m === undefined) {
      const file = path.join(this.dir, `${script}.json`);
      m = fs.existsSync(file) ? NavMesh.load(file) : null;
      this.cache.set(script, m);
    }
    return m ?? undefined;
  }
}

const clamp = (v: number, lo: number, hi: number) => (v < lo ? lo : v > hi ? hi : v);
const dist = (a: Point2, b: Point2) => Math.hypot(a.x - b.x, a.y - b.y);

/** Midpoint of the edge two touching cells share (clamped to both). */
function portal(a: Cell, b: Cell): Point2 {
  const lo = (c: Cell, k: 'x' | 'y') => c[k] - c.half;
  const hi = (c: Cell, k: 'x' | 'y') => c[k] + c.half;
  const ox0 = Math.max(lo(a, 'x'), lo(b, 'x'));
  const ox1 = Math.min(hi(a, 'x'), hi(b, 'x'));
  const oy0 = Math.max(lo(a, 'y'), lo(b, 'y'));
  const oy1 = Math.min(hi(a, 'y'), hi(b, 'y'));
  return { x: (ox0 + ox1) / 2, y: (oy0 + oy1) / 2 };
}

class MinHeap {
  private ids: number[] = [];
  private keys: number[] = [];
  get size(): number {
    return this.ids.length;
  }
  push(id: number, key: number): void {
    const ids = this.ids;
    const keys = this.keys;
    let i = ids.length;
    ids.push(id);
    keys.push(key);
    while (i > 0) {
      const p = (i - 1) >> 1;
      if (keys[p]! <= key) break;
      ids[i] = ids[p]!;
      keys[i] = keys[p]!;
      i = p;
    }
    ids[i] = id;
    keys[i] = key;
  }
  pop(): number {
    const ids = this.ids;
    const keys = this.keys;
    const top = ids[0]!;
    const lastId = ids.pop()!;
    const lastKey = keys.pop()!;
    const n = ids.length;
    if (n) {
      let i = 0;
      for (;;) {
        let c = 2 * i + 1;
        if (c >= n) break;
        if (c + 1 < n && keys[c + 1]! < keys[c]!) c++;
        if (keys[c]! >= lastKey) break;
        ids[i] = ids[c]!;
        keys[i] = keys[c]!;
        i = c;
      }
      ids[i] = lastId;
      keys[i] = lastKey;
    }
    return top;
  }
}

/** mulberry32: small, fast, good enough for wander targets. */
export function seededRng(seed: number): Rng {
  let s = seed >>> 0;
  return () => {
    s = (s + 0x6d2b79f5) >>> 0;
    let t = s;
    t = Math.imul(t ^ (t >>> 15), t | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}
