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
  /** Generators keep this many times Fable's count alive even with one hero (the shared map is small). */
  soloDensity: number;
  /** Each hero in the channel beyond the first adds this share of a generator's count, and of a placed camp. */
  perHeroDensity: number;
  /** Most a spawn point's count can grow to, as a multiple of Fable's. */
  maxDensity: number;
  /** Each hero in the channel beyond the first adds this share to a new monster's health. */
  healthPerHero: number;
  /** Most extra health a monster can get from the hero count (0.75 = +75%). */
  maxHealthBonus: number;
  /** When more heroes arrive, the extra monsters come in after this long. */
  fillDelayMs: number;
  /** Extra copies of a placed creature stand within this many metres of its spot. */
  placedSpread: number;
  /**
   * A monster only comes back while no hero is this close to its spawn spot (plus the
   * generator's radius), so nothing pops into being in front of a player. 0 = always.
   */
  hiddenRadius: number;
  rng: () => number;
  /** Optional: snap a spawn point to walkable ground (navmesh). Return null to skip that spot. */
  place?: (p: Vec3) => Vec3 | null;
}

export const DEFAULT_SPAWN_OPTIONS: SpawnOptions = {
  respawnMs: 300_000,
  generatorRespawnMs: 300_000,
  jitter: 0.25,
  generatorDefaultActive: 3,
  corpseMs: 8_000,
  soloDensity: 1.5,
  perHeroDensity: 0.5,
  maxDensity: 3,
  healthPerHero: 0.15,
  maxHealthBonus: 0.75,
  fillDelayMs: 10_000,
  placedSpread: 3,
  hiddenRadius: 30,
  rng: Math.random,
};

/** How many monsters a spawn point keeps alive for this many heroes in the channel. */
export function scaledLimit(base: number, generator: boolean, heroes: number, o: Pick<SpawnOptions, 'soloDensity' | 'perHeroDensity' | 'maxDensity'>): number {
  const extra = o.perHeroDensity * Math.max(0, heroes - 1);
  if (generator) return Math.ceil(base * Math.min(o.maxDensity, o.soloDensity * (1 + extra)));
  // Placed camps are laid out by hand in Fable: only extra heroes add copies.
  return Math.floor(base * Math.min(o.maxDensity, 1 + extra));
}

/** Health multiplier for a monster spawned with this many heroes in the channel. */
export function healthScale(heroes: number, o: Pick<SpawnOptions, 'healthPerHero' | 'maxHealthBonus'>): number {
  return 1 + Math.min(o.maxHealthBonus, o.healthPerHero * Math.max(0, heroes - 1));
}

interface Point {
  id: string;
  pool: string[];
  at: Vec3;
  yaw: number;
  radius: number;
  generator: boolean;
  /** Fable's own count for this point. */
  baseLimit: number;
  /** What it keeps alive now, after scaling for the heroes in the channel. */
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
  private heroes = 0;
  private heroPos: Vec3[] = [];

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
      this.points.set(`c:${c.uid}`, this.point(`c:${c.uid}`, [c.def], c, c.yaw, 0, false, 1, -1, this.opts.respawnMs));
    }
    for (const g of table?.generators ?? []) {
      const pool = generatorPool(g, this.data);
      if (!pool.length) continue;
      const limit = g.activeLimit > 0 ? g.activeLimit : this.opts.generatorDefaultActive;
      this.points.set(`g:${g.uid}`, this.point(`g:${g.uid}`, pool, g, 0, g.radius, true, limit, g.totalLimit, this.opts.generatorRespawnMs));
    }
  }

  get pointCount(): number {
    return this.points.size;
  }

  creatures(): IterableIterator<Creature> {
    return this.live.values();
  }

  get heroCount(): number {
    return this.heroes;
  }

  /**
   * Tell the spawns how many heroes are in the channel. More heroes means more monsters
   * (they come in after fillDelayMs) and tougher new ones. When heroes leave, the extras
   * still alive stay until killed; they just aren't replaced.
   */
  setHeroCount(n: number): void {
    if (n === this.heroes) return;
    this.heroes = n;
    for (const p of this.points.values()) {
      p.limit = scaledLimit(p.baseLimit, p.generator, n, this.opts);
      const missing = p.limit - p.live.size - p.timers.length;
      for (let i = 0; i < missing; i++) p.timers.push(this.opts.fillDelayMs);
    }
  }

  /**
   * Where the channel's heroes stand (every step). Sets the hero count for scaling and
   * holds respawns back while a hero is near a spawn spot.
   */
  setHeroes(positions: Vec3[]): void {
    this.heroPos = positions;
    this.setHeroCount(positions.length);
  }

  /** True while any hero is close enough to see this point's monsters appear. */
  private watched(p: Point): boolean {
    const r = this.opts.hiddenRadius;
    if (r <= 0) return false;
    const rr = (r + p.radius) ** 2;
    return this.heroPos.some((h) => (h.x - p.at.x) ** 2 + (h.y - p.at.y) ** 2 <= rr);
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
          if (this.watched(p)) {
            p.timers[i] = 0; // due, but waits until nobody is looking
            continue;
          }
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

  /**
   * One creature that belongs to no spawn point (a staff dev spawn): it fights and dies like
   * the rest, its body is cleared the same way, and it never comes back.
   */
  spawnExtra(def: string, at: Vec3, yaw: number): Creature | undefined {
    let pos: Vec3 = { ...at };
    if (this.opts.place) {
      const snapped = this.opts.place(pos);
      if (!snapped) return undefined;
      pos = snapped;
    }
    const c = creatureFromDef(allocCreatureId(), def, pos, this.data, null);
    c.yaw = yaw;
    this.live.set(c.id, c);
    this.combat.add(c);
    this.hooks.onSpawn?.(c);
    return c;
  }

  dispose(): void {
    for (const id of this.live.keys()) this.combat.remove(id);
    this.live.clear();
    this.corpses.clear();
    this.points.clear();
  }

  private point(id: string, pool: string[], at: Vec3, yaw: number, radius: number, generator: boolean, baseLimit: number, totalLimit: number, respawnMs: number): Point {
    const limit = scaledLimit(baseLimit, generator, this.heroes, this.opts);
    return { id, pool, at: { x: at.x, y: at.y, z: at.z }, yaw, radius, generator, baseLimit, limit, totalLimit, made: 0, live: new Set(), timers: [], respawnMs };
  }

  private canMake(p: Point): boolean {
    return p.totalLimit < 0 || p.made < p.totalLimit;
  }

  private spawn(p: Point): Creature | undefined {
    const r = this.opts.rng;
    const def = p.pool[Math.floor(r() * p.pool.length)]!;
    let at: Vec3 = { ...p.at };
    // Extra copies of a placed creature stand near its spot, not inside it.
    const radius = p.radius > 0 ? p.radius : p.live.size > 0 ? this.opts.placedSpread : 0;
    if (radius > 0) {
      const a = r() * Math.PI * 2;
      const d = Math.sqrt(r()) * radius;
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
    c.maxHealth = c.health = Math.round(c.health * healthScale(this.heroes, this.opts));
    c.yaw = radius > 0 ? r() * Math.PI * 2 - Math.PI : p.yaw;
    p.made++;
    p.live.add(c.id);
    this.live.set(c.id, c);
    this.combat.add(c);
    this.hooks.onSpawn?.(c);
    return c;
  }
}
