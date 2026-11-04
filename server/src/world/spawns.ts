import fs from 'node:fs';
import type { GameData } from '../data/game-data.js';
import { creatureFromDef } from '../combat/loadout.js';
import type { CombatSystem } from '../combat/combat.js';
import { CREATURE_ID_BASE, type AiHooks, type Creature } from './entities.js';
import type { Vec3 } from './sim.js';

/** shared/defs/spawns.json, written by tools/extractor/spawns.mjs from the operator's install. */
export interface PlacedCreature {
  uid: string;
  def: string;
  x: number;
  y: number;
  z: number;
  yaw: number;
  script: string | null;
}
export interface GeneratorDef {
  uid: string;
  x: number;
  y: number;
  z: number;
  families: string[];
  radius: number;
  triggerRadius: number;
  activeLimit: number;
  totalLimit: number;
  script: string | null;
}
export interface SpawnTable {
  creatures: PlacedCreature[];
  generators: GeneratorDef[];
}
export interface SpawnsFile {
  regions: Record<string, SpawnTable>;
}

export function loadSpawns(file: string | undefined): SpawnsFile {
  if (!file || !fs.existsSync(file)) return { regions: {} };
  const d = JSON.parse(fs.readFileSync(file, 'utf8')) as SpawnsFile;
  return { regions: d.regions ?? {} };
}

/** Only these factions are spawned by the server. Villagers, traders and quest people stay with the client. */
export const HOSTILE_FACTIONS = new Set(['FACTION_MONSTERS', 'FACTION_BANDITS']);

/**
 * A placed creature the server spawns: hostile, and not named by a quest script. Quests run
 * in each player's own game and find their creatures by ScriptName, so those stay local.
 */
export function serverRunsPlaced(c: PlacedCreature, data: GameData): boolean {
  return !c.script && HOSTILE_FACTIONS.has(data.creatures[c.def]?.faction ?? '');
}

/** The hostile defs a generator's families can make. */
export function generatorPool(g: GeneratorDef, data: GameData): string[] {
  return g.families.flatMap((f) => data.families[f] ?? []).filter((d) => HOSTILE_FACTIONS.has(data.creatures[d]?.faction ?? ''));
}

/**
 * Every creature def the server spawns somewhere, sorted. Sent to the mod in its welcome so
 * the local level loader leaves its own copies of these out (client-mod/src/game/spawns.cpp).
 */
export function serverSpawnDefs(spawns: SpawnsFile, data: GameData): string[] {
  const defs = new Set<string>();
  for (const t of Object.values(spawns.regions)) {
    for (const c of t.creatures ?? []) if (serverRunsPlaced(c, data)) defs.add(c.def);
    for (const g of t.generators ?? []) for (const d of generatorPool(g, data)) defs.add(d);
  }
  return [...defs].sort();
}

export interface SpawnOptions {
  /** A placed creature comes back this long after it dies. */
  respawnMs: number;
  /** A generator refills one slot this long after a creature from it dies. */
  generatorRespawnMs: number;
  /** +/- share of randomness on every respawn timer, so a camp doesn't pop back all at once. */
  jitter: number;
  /** Generators with no limit in the level file keep this many alive. */
  generatorDefaultActive: number;
  /** A body stays in the world this long before it is removed. */
  corpseMs: number;
  rng: () => number;
  /** Optional: snap a spawn point to walkable ground (navmesh). Return null to skip that spot. */
  place?: (p: Vec3) => Vec3 | null;
}

export const DEFAULT_SPAWN_OPTIONS: SpawnOptions = {
  respawnMs: 120_000,
  generatorRespawnMs: 60_000,
  jitter: 0.25,
  generatorDefaultActive: 3,
  corpseMs: 8_000,
  rng: Math.random,
};

interface Point {
  id: string;
  pool: string[];
  at: Vec3;
  yaw: number;
  radius: number;
  limit: number;
  totalLimit: number;
  made: number;
  live: Set<number>;
  /** ms left on each pending respawn. */
  timers: number[];
  respawnMs: number;
}

let nextCreatureId = CREATURE_ID_BASE;
/** Ids are unique for the whole process, so loot keys and AI state never mix two creatures. */
export const allocCreatureId = () => nextCreatureId++;

/**
 * Keeps one channel's monsters alive: placed creatures respawn where the level put them,
 * creature generators keep a number of creatures from their families around. Each channel
 * has its own copy, so two channels of the same region don't share monsters.
 */
export class SpawnManager {
  private points = new Map<string, Point>();
  private live = new Map<number, Creature>();
  private corpses = new Map<number, number>();
  private opts: SpawnOptions;

  constructor(
    table: SpawnTable | undefined,
    private data: GameData,
    private combat: CombatSystem,
    private hooks: AiHooks = {},
    opts: Partial<SpawnOptions> = {},
  ) {
    this.opts = { ...DEFAULT_SPAWN_OPTIONS, ...opts };
    for (const c of table?.creatures ?? []) {
      if (!serverRunsPlaced(c, this.data)) continue;
      this.points.set(`c:${c.uid}`, this.point(`c:${c.uid}`, [c.def], c, c.yaw, 0, 1, -1, this.opts.respawnMs));
    }
    for (const g of table?.generators ?? []) {
      const pool = generatorPool(g, this.data);
      if (!pool.length) continue;
      const limit = g.activeLimit > 0 ? g.activeLimit : this.opts.generatorDefaultActive;
      this.points.set(`g:${g.uid}`, this.point(`g:${g.uid}`, pool, g, 0, g.radius, limit, g.totalLimit, this.opts.generatorRespawnMs));
    }
  }

  get pointCount(): number {
    return this.points.size;
  }

  creatures(): IterableIterator<Creature> {
    return this.live.values();
  }

  /** Fill every point up to its limit. */
  start(): void {
    for (const p of this.points.values()) while (p.live.size < p.limit && this.canMake(p)) this.spawn(p);
  }

  step(dtMs: number): void {
    for (const p of this.points.values()) {
      for (let i = p.timers.length - 1; i >= 0; i--) {
        p.timers[i]! -= dtMs;
        if (p.timers[i]! <= 0) {
          p.timers.splice(i, 1);
          if (p.live.size < p.limit && this.canMake(p)) this.spawn(p);
        }
      }
    }
    for (const [id, ms] of this.corpses) {
      if (ms - dtMs > 0) this.corpses.set(id, ms - dtMs);
      else {
        this.corpses.delete(id);
        this.live.delete(id);
        this.combat.remove(id);
      }
    }
  }

  /** Call when a creature dies. Schedules the body's removal and the respawn. */
  died(creatureId: number): void {
    const c = this.live.get(creatureId);
    if (!c) return;
    this.corpses.set(c.id, this.opts.corpseMs);
    const p = c.spawnId ? this.points.get(c.spawnId) : undefined;
    if (!p || !p.live.delete(c.id)) return;
    const j = 1 + (this.opts.rng() * 2 - 1) * this.opts.jitter;
    p.timers.push(p.respawnMs * j);
  }

  dispose(): void {
    for (const id of this.live.keys()) this.combat.remove(id);
    this.live.clear();
    this.corpses.clear();
    this.points.clear();
  }

  private point(id: string, pool: string[], at: Vec3, yaw: number, radius: number, limit: number, totalLimit: number, respawnMs: number): Point {
    return { id, pool, at: { x: at.x, y: at.y, z: at.z }, yaw, radius, limit, totalLimit, made: 0, live: new Set(), timers: [], respawnMs };
  }

  private canMake(p: Point): boolean {
    return p.totalLimit < 0 || p.made < p.totalLimit;
  }

  private spawn(p: Point): Creature | undefined {
    const r = this.opts.rng;
    const def = p.pool[Math.floor(r() * p.pool.length)]!;
    let at: Vec3 = { ...p.at };
    if (p.radius > 0) {
      const a = r() * Math.PI * 2;
      const d = Math.sqrt(r()) * p.radius;
      at = { x: p.at.x + Math.cos(a) * d, y: p.at.y + Math.sin(a) * d, z: p.at.z };
    }
    if (this.opts.place) {
      const snapped = this.opts.place(at);
      if (!snapped) {
        p.timers.push(5_000); // try again shortly at another spot
        return undefined;
      }
      at = snapped;
    }
    const c = creatureFromDef(allocCreatureId(), def, at, this.data, p.id);
    c.yaw = p.radius > 0 ? r() * Math.PI * 2 - Math.PI : p.yaw;
    p.made++;
    p.live.add(c.id);
    this.live.set(c.id, c);
    this.combat.add(c);
    this.hooks.onSpawn?.(c);
    return c;
  }
}
