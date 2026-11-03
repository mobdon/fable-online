import type { NavMesh } from '../nav/navmesh.js';
import type { Vec3 } from '../sim.js';

/**
 * A server-run creature. This mirrors the shared entity model in world/entities.ts that
 * the combat module owns (same field names and meaning); once both land on main this
 * becomes an import from there. Entity ids start at 1,000,000 so they never clash with
 * hero ids.
 */
export interface Creature {
  id: number;
  /** Fable def name, e.g. CREATURE_BANDIT_GRUNT_LEVEL1. Picks the AI profile. */
  defName: string;
  kind: 'creature';
  pos: Vec3;
  yaw: number;
  health: number;
  maxHealth: number;
  faction: string;
  alive: boolean;
  anim: number;
  targetId: number | null;
  spawnId: string | null;
  /** Set by combat after a swing; the AI never swings while it is above 0. */
  attackCooldownMs: number;
  blocking: boolean;
  /** Set by combat on a stagger; the AI neither moves nor swings while it is above 0. */
  stunnedMs: number;
  /** Optional per-def stats from game.bin; the family profile fills any gap. */
  stats?: {
    walkSpeed?: number;
    runSpeed?: number;
    sightRange?: number;
    attackRange?: number;
    flees?: boolean;
  };
}

export type AttackKind = 'light' | 'heavy' | 'flourish' | 'ranged';

/** What the AI asks of combat. Combat decides if a swing lands and for how much. */
export interface CombatPort {
  attack(attackerId: number, targetId: number, kind: AttackKind): void;
  setBlocking(id: number, on: boolean): void;
}

/** Calls combat and spawns make into the AI. */
export interface AiHooks {
  onSpawn(c: Creature): void;
  onDamaged(creatureId: number, attackerId: number, amount: number): void;
  onDeath(creatureId: number): void;
}

/** Anything a creature can fight: a hero, or (later) a guard or another creature. */
export interface Combatant {
  id: number;
  pos: Vec3;
  alive: boolean;
  faction: string;
}

/** The slice of a channel's world the AI reads each tick. */
export interface AiWorld {
  /** Undefined when no navmesh was extracted for the region: creatures then move in straight lines. */
  nav: NavMesh | undefined;
  creatures: ReadonlyMap<number, Creature>;
  /** Heroes in this channel (and anything else creatures may attack). */
  targets(): Iterable<Combatant>;
  combat: CombatPort;
  /**
   * Who this creature treats as an enemy. Defaults to "any other faction". Guards use it so
   * they only go after heroes the law module has marked as wanted or resisting.
   */
  isHostile?(c: Creature, t: Combatant): boolean;
}

/**
 * Animation ids sent in EntityState.anim. The client mod maps each one to the creature's
 * own Fable animation (see docs/creature-sync.md). Combat may overwrite anim with its own
 * ids for hits and deaths.
 */
export const Anim = {
  Idle: 0,
  Walk: 1,
  Run: 2,
  Attack: 3,
  Alert: 4,
  Hit: 5,
  Death: 6,
  Leap: 7,
  Strafe: 8,
  Block: 9,
  Taunt: 10,
} as const;
