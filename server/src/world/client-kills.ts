import type { GameData } from '../data/game-data.js';
import { generatorPool, serverRunsPlaced, type SpawnTable } from './spawns.js';
import type { Vec3 } from './sim.js';

/**
 * Monsters run in each player's own Fable (FO_CLIENT_MONSTERS). The mod reports every kill
 * its game makes; the server only pays for kills that fit the region's own spawn table:
 * a placed monster pays once per respawn time, a generator pays its active count per
 * respawn time. Anything else (a forced respawn, a monster the level never places) is still
 * killed in that player's game but pays nothing.
 */
export interface ClientKillOptions {
  /** A placed monster pays again this long after its last paid kill. */
  respawnMs: number;
  /** Each generator pays its active count within this window. */
  generatorRespawnMs: number;
  /** Generators with no limit in the level file pay this many per window. */
  generatorDefaultActive: number;
  /** A kill this far from a placed monster's spot (or a generator's radius) still counts as it. */
  wanderM: number;
  /** The hero must be this close to the kill (server's own position). */
  heroReachM: number;
}

export const DEFAULT_CLIENT_KILL_OPTIONS: ClientKillOptions = {
  respawnMs: 300_000,
  generatorRespawnMs: 300_000,
  generatorDefaultActive: 3,
  wanderM: 30,
  heroReachM: 40,
};

interface Slot {
  key: string;
  defs: Set<string>;
  x: number;
  y: number;
  radius: number;
  cap: number;
  windowMs: number;
  paid: number[];
}

export type KillClaim = { ok: true; slot: string } | { ok: false; reason: 'unknown_def' | 'hero_far' | 'no_spot' | 'spent' };

export class ClientKillLedger {
  private readonly slots: Slot[] = [];

  constructor(
    table: SpawnTable | undefined,
    data: GameData,
    private readonly o: ClientKillOptions = DEFAULT_CLIENT_KILL_OPTIONS,
  ) {
    for (const c of table?.creatures ?? []) {
      if (!serverRunsPlaced(c, data)) continue;
      this.slots.push({ key: c.uid, defs: new Set([c.def]), x: c.x, y: c.y, radius: o.wanderM, cap: 1, windowMs: o.respawnMs, paid: [] });
    }
    for (const g of table?.generators ?? []) {
      const pool = generatorPool(g, data);
      if (!pool.length) continue;
      this.slots.push({
        key: g.uid,
        defs: new Set(pool),
        x: g.x,
        y: g.y,
        radius: (g.radius || 0) + o.wanderM,
        cap: g.activeLimit > 0 ? g.activeLimit : o.generatorDefaultActive,
        windowMs: o.generatorRespawnMs,
        paid: [],
      });
    }
  }

  /** Is this kill (in the region's own frame) one the server pays for? Records it if so. */
  claim(def: string, at: Vec3, hero: Vec3 | undefined, now: number): KillClaim {
    const known = this.slots.some((s) => s.defs.has(def));
    if (!known) return { ok: false, reason: 'unknown_def' };
    if (!hero || Math.hypot(hero.x - at.x, hero.y - at.y) > this.o.heroReachM) return { ok: false, reason: 'hero_far' };
    let best: Slot | undefined;
    let bestD = Infinity;
    let near = false;
    for (const s of this.slots) {
      if (!s.defs.has(def)) continue;
      const d = Math.hypot(s.x - at.x, s.y - at.y);
      if (d > s.radius) continue;
      near = true;
      s.paid = s.paid.filter((t) => now - t < s.windowMs);
      if (s.paid.length >= s.cap) continue;
      if (d < bestD) [best, bestD] = [s, d];
    }
    if (!best) return { ok: false, reason: near ? 'spent' : 'no_spot' };
    best.paid.push(now);
    return { ok: true, slot: best.key };
  }
}
