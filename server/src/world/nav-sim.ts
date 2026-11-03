import { AiDirector } from './ai/brain.js';
import { Anim, type AttackKind, type AiWorld, type Combatant, type CombatPort, type Creature } from './ai/types.js';
import type { NavMesh, NavStore } from './nav/navmesh.js';
import type { RegionDef } from './regions.js';
import { StubSim, type EntityState, type PlayerInput, type RegionSim, type SimFactory, type Vec3 } from './sim.js';

export const FIRST_CREATURE_ID = 1_000_000;

interface Hero {
  pos: Vec3;
  yaw: number;
  move: [number, number];
  anim: number;
  health: number;
}

/**
 * A region simulated by the server itself, no Fable.exe: heroes walk on the region's
 * navmesh (they can't walk through walls or off cliffs, and z follows the ground) and
 * creatures are run by the AI.
 *
 * Combat is the combat module's job. Until it is wired in, `combat` defaults to
 * StubCombat, which applies flat damage so the AI can be exercised end to end.
 */
export class NavSim implements RegionSim {
  static readonly RUN_SPEED = StubSim.RUN_SPEED;
  readonly creatures = new Map<number, Creature>();
  readonly ai: AiDirector;
  private heroes = new Map<number, Hero>();
  private nextId = FIRST_CREATURE_ID;
  private world: AiWorld;

  constructor(
    readonly nav: NavMesh,
    combat?: CombatPort,
    seed = 1,
  ) {
    const self = this;
    this.world = {
      nav,
      creatures: this.creatures,
      combat: combat ?? new StubCombat(this),
      *targets(): Iterable<Combatant> {
        for (const [id, h] of self.heroes) yield { id, pos: h.pos, alive: h.health > 0, faction: 'hero' };
      },
    };
    this.ai = new AiDirector(this.world, seed);
  }

  // ---------- heroes ----------

  addHero(heroId: number, at: Vec3): void {
    const p = this.nav.snap(at, 64) ?? this.anyGround();
    this.heroes.set(heroId, { pos: p, yaw: 0, move: [0, 0], anim: 0, health: 100 });
  }
  removeHero(heroId: number): Vec3 | undefined {
    const h = this.heroes.get(heroId);
    this.heroes.delete(heroId);
    return h?.pos;
  }
  applyInput(heroId: number, input: PlayerInput): void {
    const h = this.heroes.get(heroId);
    if (!h) return;
    h.yaw = input.cameraYaw;
    h.move = [input.moveX, input.moveY];
  }
  position(heroId: number): Vec3 | undefined {
    const p = this.heroes.get(heroId)?.pos;
    return p && { ...p };
  }
  hero(heroId: number): Hero | undefined {
    return this.heroes.get(heroId);
  }

  // ---------- creatures ----------

  spawn(defName: string, at: Vec3, init: Partial<Creature> = {}): Creature {
    const pos = this.nav.snap(at, 8);
    if (!pos) throw new Error(`no ground near ${at.x},${at.y} in ${this.nav.script}`);
    const c: Creature = {
      id: this.nextId++,
      defName,
      kind: 'creature',
      pos,
      yaw: 0,
      health: 100,
      maxHealth: 100,
      faction: 'monster',
      alive: true,
      anim: Anim.Idle,
      targetId: null,
      spawnId: null,
      attackCooldownMs: 0,
      blocking: false,
      stunnedMs: 0,
      ...init,
    };
    this.creatures.set(c.id, c);
    this.ai.onSpawn(c);
    return c;
  }

  despawn(id: number): void {
    this.creatures.delete(id);
    this.ai.remove(id);
  }

  // ---------- tick ----------

  step(dtMs: number): void {
    const d = (NavSim.RUN_SPEED * dtMs) / 1000;
    for (const h of this.heroes.values()) {
      const [mx, my] = h.move;
      if (mx === 0 && my === 0) {
        h.anim = 0;
        continue;
      }
      const s = Math.sin(h.yaw);
      const c = Math.cos(h.yaw);
      const nx = h.pos.x + (mx * c - my * s) * d;
      const ny = h.pos.y + (mx * s + my * c) * d;
      // Slide along walls: try the full step, then each axis on its own.
      if (this.nav.walkable(nx, ny)) {
        h.pos.x = nx;
        h.pos.y = ny;
      } else if (this.nav.walkable(nx, h.pos.y)) h.pos.x = nx;
      else if (this.nav.walkable(h.pos.x, ny)) h.pos.y = ny;
      h.pos.z = this.nav.heightAt(h.pos.x, h.pos.y);
      h.anim = 1;
    }
    for (const c of this.creatures.values()) {
      c.attackCooldownMs = Math.max(0, c.attackCooldownMs - dtMs);
      c.stunnedMs = Math.max(0, c.stunnedMs - dtMs);
    }
    this.ai.step(dtMs);
  }

  snapshot(): EntityState[] {
    const out: EntityState[] = [];
    for (const [id, h] of this.heroes)
      out.push({ id, def_id: 0, x: round(h.pos.x), y: round(h.pos.y), z: round(h.pos.z), yaw: round(h.yaw), anim: h.anim, health: h.health });
    for (const c of this.creatures.values())
      out.push({ id: c.id, def_id: 0, def: c.defName, x: round(c.pos.x), y: round(c.pos.y), z: round(c.pos.z), yaw: round(c.yaw), anim: c.anim, health: Math.round(c.health) });
    return out;
  }

  dispose(): void {
    this.heroes.clear();
    this.creatures.clear();
  }

  private anyGround(): Vec3 {
    const c = this.nav.cells[0]!;
    return { x: c.x, y: c.y, z: this.nav.heightAt(c.x, c.y) };
  }
}

/** Flat damage stand-in for the combat module: 10 per hit, 1.2 s between swings. */
export class StubCombat implements CombatPort {
  readonly log: { attacker: number; target: number; kind: AttackKind }[] = [];
  constructor(private sim: NavSim) {}
  attack(attackerId: number, targetId: number, kind: AttackKind): void {
    const a = this.sim.creatures.get(attackerId);
    if (!a || a.attackCooldownMs > 0) return;
    a.attackCooldownMs = 1200;
    this.log.push({ attacker: attackerId, target: targetId, kind });
    const h = this.sim.hero(targetId);
    if (h) h.health = Math.max(0, h.health - (kind === 'heavy' ? 15 : 10));
  }
  setBlocking(id: number, on: boolean): void {
    const c = this.sim.creatures.get(id);
    if (c) c.blocking = on;
  }
}

/** Use the navmesh sim for regions that have one, the kinematic stub for the rest. */
export function navSimFactory(store: NavStore): SimFactory {
  return (region: RegionDef) => {
    const nav = store.get(region.name);
    return nav ? new NavSim(nav) : new StubSim();
  };
}

const round = (n: number) => Math.round(n * 100) / 100;
