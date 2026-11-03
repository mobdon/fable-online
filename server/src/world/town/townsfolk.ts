import type { EntityState, Vec3 } from '../sim.js';
import { fableHash } from '../../data/fable-bin.js';
import type { WorldClock } from '../clock.js';
import type { PlacedHome, PlacedShop, TownData, TownRegion, VillagePeople, VillageRole } from './town-data.js';

/**
 * Villagers and shopkeepers, run by the server. Kept deliberately simple: they stand, walk
 * about their village by day, go indoors at night, run from fights, and shopkeepers mind their
 * shop. No dialogue, no opinions, no quests (quests stay with each player's own Fable).
 *
 * Everyone in a channel sees the same villagers in the same places, which the guard and crime
 * system needs for witnesses. Each channel has its own copy, like monsters.
 */

/** Town folk ids start here: above hero ids and far above creature ids (from 1,000,000). */
export const NPC_ID_BASE = 900_000_000;

/**
 * Animation ids in EntityState.anim for town folk. 0..2 match heroes and creatures; the
 * client maps the rest to the villager's own Fable animations (docs/town-life.md).
 */
export const NpcAnim = { Idle: 0, Walk: 1, Run: 2, Cower: 11, Greet: 12, Work: 13 } as const;

export type NpcRole = 'shopkeeper' | 'barkeep' | 'barmaid' | 'worker' | 'villager' | 'trader';
export type NpcState = 'idle' | 'walk' | 'work' | 'flee' | 'cower' | 'home' | 'indoors';

export interface Npc {
  id: number;
  def: string;
  role: NpcRole;
  village: string | null;
  pos: Vec3;
  yaw: number;
  anim: number;
  state: NpcState;
  /** Where it sleeps. Town folk without a house sleep at their post. */
  home: Vec3;
  /** Where it works (shop counter) or hangs about (village square). */
  post: Vec3;
  /** Set for shopkeepers: the shop they mind. */
  shopId: string | null;
  /** Stays out at night. */
  nightOwl: boolean;
}

/** What town folk walk on. The navmesh fits this shape; without one they walk in straight lines on flat ground. */
export interface Ground {
  walkable(x: number, y: number): boolean;
  heightAt(x: number, y: number): number;
  findPath?(a: { x: number; y: number }, b: { x: number; y: number }): { x: number; y: number }[] | undefined;
}

export interface TownOptions {
  /** Most residents a village spawns besides its shopkeepers. */
  maxResidents: number;
  /** Residents for a village with no houses (travellers on the roads). */
  homelessResidents: number;
  wanderRadius: number;
  walkSpeed: number;
  runSpeed: number;
  /** A fight this close makes villagers run and shopkeepers duck. */
  dangerRadius: number;
  fleeMs: number;
  cowerMs: number;
  /** An idle villager turns to face a hero this close. */
  greetRadius: number;
  /** Share of villagers who stay out after dark. */
  nightOwlShare: number;
  idleMs: [number, number];
  rng: () => number;
}

export const DEFAULT_TOWN_OPTIONS: TownOptions = {
  maxResidents: 12,
  homelessResidents: 2,
  wanderRadius: 18,
  walkSpeed: 1.4,
  runSpeed: 4.5,
  dangerRadius: 10,
  fleeMs: 6000,
  cowerMs: 4000,
  greetRadius: 4,
  nightOwlShare: 0.2,
  idleMs: [3000, 9000],
  rng: Math.random,
};

const THINK_MS = 250;
const ARRIVE = 0.4;
/** A shopkeeper further than this from the counter walks back to it. */
const POST_SLACK = 1.5;
const FALLBACK_KEEPER = 'CREATURE_TRADER_01';

export const shopIdOf = (region: string, uid: string) => `${region}:${uid}`;

interface Mind {
  npc: Npc;
  until: number;
  nextThink: number;
  path: { x: number; y: number; z: number }[];
  speed: number;
  /** Where the last scare came from, to run away from it. */
  scare: Vec3 | null;
}

let nextNpcId = NPC_ID_BASE;

export class TownLife {
  readonly npcs = new Map<number, Npc>();
  private minds = new Map<number, Mind>();
  private byShop = new Map<string, Npc>();
  private opts: TownOptions;
  private now = 0;

  constructor(
    readonly region: string,
    data: TownData,
    private clock: WorldClock,
    private ground?: Ground,
    opts: Partial<TownOptions> = {},
  ) {
    this.opts = { ...DEFAULT_TOWN_OPTIONS, ...opts };
    const r = data.regions[region];
    if (r) this.populate(r, data.villages);
  }

  // ---------------------------------------------------------------- queries

  /** A shop trades only while its keeper is behind the counter (not hiding from a fight). */
  shopOpen(shopId: string): boolean {
    const k = this.byShop.get(shopId);
    return !!k && (k.state === 'work' || k.state === 'idle');
  }

  keeperOf(shopId: string): Npc | undefined {
    return this.byShop.get(shopId);
  }

  /** Town folk out of doors within `radius` of a point: who could see a crime there. */
  witnesses(at: Vec3, radius: number): Npc[] {
    const out: Npc[] = [];
    for (const n of this.npcs.values()) if (n.state !== 'indoors' && dist(n.pos, at) <= radius) out.push(n);
    return out;
  }

  /** Something frightening happened here (a fight, a crime, a spell). */
  scare(at: Vec3, radius = this.opts.dangerRadius): void {
    for (const m of this.minds.values()) {
      const n = m.npc;
      if (n.state === 'indoors' || dist(n.pos, at) > radius) continue;
      if (n.role === 'shopkeeper' || n.role === 'barkeep' || n.role === 'barmaid') {
        this.set(m, 'cower', this.opts.cowerMs);
        n.anim = NpcAnim.Cower;
        m.path = [];
      } else if (n.state !== 'flee') {
        m.scare = { ...at };
        this.set(m, 'flee', this.opts.fleeMs);
        this.runFrom(m, at);
      }
    }
  }

  snapshot(): EntityState[] {
    const out: EntityState[] = [];
    for (const n of this.npcs.values()) {
      if (n.state === 'indoors') continue;
      out.push({ id: n.id, def_id: fableHash(n.def), def: n.def, x: r2(n.pos.x), y: r2(n.pos.y), z: r2(n.pos.z), yaw: r2(n.yaw), anim: n.anim, health: 1 });
    }
    return out;
  }

  // ---------------------------------------------------------------- tick

  /** `heroes` are the players in the channel; `dangers` are live fights (hostile creatures, swings, spells). */
  step(dtMs: number, heroes: Iterable<Vec3>, dangers: Iterable<Vec3>): void {
    this.now += dtMs;
    for (const d of dangers) this.scare(d);
    const night = this.clock.isNight();
    const heroList = [...heroes];
    for (const m of this.minds.values()) {
      if (this.now >= m.nextThink) {
        m.nextThink = this.now + THINK_MS;
        this.think(m, night, heroList);
      }
      this.move(m, dtMs);
    }
  }

  dispose(): void {
    this.npcs.clear();
    this.minds.clear();
    this.byShop.clear();
  }

  // ---------------------------------------------------------------- setup

  private populate(r: TownRegion, villages: Record<string, VillagePeople>): void {
    const villageDef = new Map(r.villages.map((v) => [v.uid, v.def]));
    const people = (uid: string | null): VillagePeople | undefined => {
      const def = (uid && villageDef.get(uid)) ?? r.villages[0]?.def;
      return def ? villages[def] : undefined;
    };
    const role = (uid: string | null, want: VillageRole) => people(uid)?.roles[want];

    for (const s of r.shops) {
      const tavern = /TAVERN|INN|BORDELLO|FEASTING|DINING/.test(s.building);
      const at = front(s, 2);
      const keeper = this.add(tavern ? 'barkeep' : 'shopkeeper', (tavern ? role(s.village, 'MaleBarman') : role(s.village, 'MaleShopKeeper')) ?? FALLBACK_KEEPER, s.village, at, at, s.yaw);
      keeper.shopId = shopIdOf(this.region, s.uid);
      keeper.state = 'work';
      keeper.anim = NpcAnim.Work;
      this.byShop.set(keeper.shopId, keeper);
      const maid = tavern && role(s.village, 'FemaleBarmaid');
      if (maid) this.add('barmaid', maid, s.village, front(s, 3.5), front(s, 3.5), s.yaw);
    }

    for (const v of r.villages) {
      const p = villages[v.def];
      if (!p) continue;
      const homes = r.homes.filter((h) => h.village === v.uid);
      const kinds = [p.roles.MaleUnemployed, p.roles.FemaleUnemployed].filter((d): d is string => !!d);
      if (!kinds.length) continue;
      const n = homes.length ? Math.min(this.opts.maxResidents, homes.length) : this.opts.homelessResidents;
      const square = { x: v.x, y: v.y, z: v.z };
      for (let i = 0; i < n; i++) {
        const home: Vec3 = homes.length ? doorOf(homes[i % homes.length]!) : square;
        const npc = this.add('villager', kinds[i % kinds.length]!, v.uid, home, square, this.opts.rng() * Math.PI * 2 - Math.PI);
        npc.nightOwl = this.opts.rng() < this.opts.nightOwlShare;
      }
      if (p.roles.MaleWarehouseWorker && homes.length) {
        const h = doorOf(homes[0]!);
        this.add('worker', p.roles.MaleWarehouseWorker, v.uid, h, h, 0);
      }
    }

    for (const pv of r.villagers) {
      const at = { x: pv.x, y: pv.y, z: pv.z };
      this.add(/TRADER/.test(pv.def) ? 'trader' : 'villager', pv.def, pv.village, at, at, pv.yaw);
    }
  }

  private add(role: NpcRole, def: string, village: string | null, home: Vec3, post: Vec3, yaw: number): Npc {
    const start = { ...post };
    const npc: Npc = { id: nextNpcId++, def, role, village, pos: start, yaw, anim: NpcAnim.Idle, state: 'idle', home: { ...home }, post: { ...post }, shopId: null, nightOwl: false };
    this.npcs.set(npc.id, npc);
    this.minds.set(npc.id, { npc, until: this.now + this.between(this.opts.idleMs), nextThink: this.opts.rng() * THINK_MS, path: [], speed: 0, scare: null });
    return npc;
  }

  // ---------------------------------------------------------------- behaviour

  private think(m: Mind, night: boolean, heroes: Vec3[]): void {
    const n = m.npc;
    switch (n.state) {
      case 'flee':
        if (this.now >= m.until) {
          this.set(m, 'cower', this.opts.cowerMs);
          n.anim = NpcAnim.Cower;
          m.path = [];
        } else if (!m.path.length && m.scare) this.runFrom(m, m.scare);
        return;
      case 'cower':
        if (this.now >= m.until) this.resume(m);
        return;
      case 'indoors':
        if (!night) {
          n.pos = { ...n.home };
          this.resume(m);
        }
        return;
      case 'home':
        if (!m.path.length) {
          n.state = 'indoors';
          n.anim = NpcAnim.Idle;
        }
        return;
      case 'work': {
        if (dist(n.pos, n.post) > POST_SLACK) return this.walkTo(m, n.post, this.opts.walkSpeed, 'work');
        const h = nearest(n.pos, heroes, this.opts.greetRadius * 2);
        if (h) n.yaw = faceYaw(n.pos, h);
        n.anim = h ? NpcAnim.Greet : NpcAnim.Work;
        return;
      }
      case 'walk':
        if (!m.path.length) this.idle(m);
        return;
      case 'idle': {
        if (this.keepsPost(n)) return this.resume(m);
        if (night && !n.nightOwl) return this.walkTo(m, n.home, this.opts.walkSpeed, 'home');
        const h = nearest(n.pos, heroes, this.opts.greetRadius);
        if (h) {
          n.yaw = faceYaw(n.pos, h);
          n.anim = NpcAnim.Greet;
          return;
        }
        n.anim = NpcAnim.Idle;
        if (this.now >= m.until) {
          const to = this.wanderTarget(n);
          if (to) this.walkTo(m, to, this.opts.walkSpeed, 'walk');
          else m.until = this.now + this.between(this.opts.idleMs);
        }
        return;
      }
    }
  }

  private keepsPost(n: Npc): boolean {
    return n.role === 'shopkeeper' || n.role === 'barkeep';
  }

  private resume(m: Mind): void {
    m.scare = null;
    if (this.keepsPost(m.npc)) {
      m.npc.state = 'work';
      m.npc.anim = NpcAnim.Work;
    } else this.idle(m);
  }

  private idle(m: Mind): void {
    this.set(m, 'idle', this.between(this.opts.idleMs));
    m.npc.anim = NpcAnim.Idle;
    m.path = [];
  }

  private set(m: Mind, s: NpcState, forMs: number): void {
    m.npc.state = s;
    m.until = this.now + forMs;
  }

  private wanderTarget(n: Npc): Vec3 | undefined {
    const r = this.opts.rng;
    for (let i = 0; i < 6; i++) {
      const a = r() * Math.PI * 2;
      const d = Math.sqrt(r()) * this.opts.wanderRadius;
      const x = n.post.x + Math.cos(a) * d;
      const y = n.post.y + Math.sin(a) * d;
      if (this.ground && !this.ground.walkable(x, y)) continue;
      return { x, y, z: this.ground ? this.ground.heightAt(x, y) : n.post.z };
    }
    return undefined;
  }

  private walkTo(m: Mind, to: Vec3, speed: number, state: NpcState): void {
    const n = m.npc;
    const path = this.ground?.findPath?.(n.pos, to);
    m.path = path?.length ? path.map((p) => ({ x: p.x, y: p.y, z: this.ground!.heightAt(p.x, p.y) })) : [{ ...to }];
    m.speed = speed;
    n.state = state;
    n.anim = speed > this.opts.walkSpeed ? NpcAnim.Run : NpcAnim.Walk;
  }

  private runFrom(m: Mind, from: Vec3): void {
    const n = m.npc;
    let dx = n.pos.x - from.x;
    let dy = n.pos.y - from.y;
    const len = Math.hypot(dx, dy) || 1;
    if (len < 0.01) [dx, dy] = [Math.cos(n.yaw), Math.sin(n.yaw)];
    const r = this.opts.dangerRadius + 4;
    for (const turn of [0, 0.6, -0.6, 1.2, -1.2]) {
      const c = Math.cos(turn);
      const s = Math.sin(turn);
      const x = n.pos.x + ((dx * c - dy * s) / len) * r;
      const y = n.pos.y + ((dx * s + dy * c) / len) * r;
      if (this.ground && !this.ground.walkable(x, y)) continue;
      this.walkTo(m, { x, y, z: this.ground ? this.ground.heightAt(x, y) : n.pos.z }, this.opts.runSpeed, 'flee');
      return;
    }
    // Nowhere to run: hide where they stand.
    this.set(m, 'cower', this.opts.cowerMs);
    n.anim = NpcAnim.Cower;
  }

  private move(m: Mind, dtMs: number): void {
    const n = m.npc;
    let step = (m.speed * dtMs) / 1000;
    while (m.path.length && step > 0) {
      const t = m.path[0]!;
      const dx = t.x - n.pos.x;
      const dy = t.y - n.pos.y;
      const d = Math.hypot(dx, dy);
      if (d <= Math.max(ARRIVE, step)) {
        n.pos = { ...t };
        m.path.shift();
        step -= d;
        continue;
      }
      n.yaw = Math.atan2(-dx, dy);
      n.pos.x += (dx / d) * step;
      n.pos.y += (dy / d) * step;
      if (this.ground) n.pos.z = this.ground.heightAt(n.pos.x, n.pos.y);
      step = 0;
    }
    if (!m.path.length && (n.anim === NpcAnim.Walk || n.anim === NpcAnim.Run)) n.anim = n.state === 'flee' ? NpcAnim.Cower : NpcAnim.Idle;
  }

  /** A random duration in [a, b] ms. */
  private between([a, b]: [number, number]): number {
    return a + this.opts.rng() * (b - a);
  }
}

/** A point `d` metres in front of a building, where its keeper stands. */
function front(s: PlacedShop, d: number): Vec3 {
  return { x: s.x - Math.sin(s.yaw) * d, y: s.y + Math.cos(s.yaw) * d, z: s.z };
}

/** Building origins sit inside the walls; residents come and go from just outside. */
function doorOf(h: PlacedHome): Vec3 {
  return { x: h.x, y: h.y - 3, z: h.z };
}

const dist = (a: { x: number; y: number }, b: { x: number; y: number }) => Math.hypot(a.x - b.x, a.y - b.y);
const r2 = (n: number) => Math.round(n * 100) / 100;
/** Same yaw convention as the .tng files and the hero: 0 faces +y. */
const faceYaw = (from: Vec3, to: Vec3) => Math.atan2(-(to.x - from.x), to.y - from.y);

function nearest(p: Vec3, list: Vec3[], within: number): Vec3 | undefined {
  let best: Vec3 | undefined;
  let bd = within;
  for (const q of list) {
    const d = dist(p, q);
    if (d <= bd) [best, bd] = [q, d];
  }
  return best;
}
