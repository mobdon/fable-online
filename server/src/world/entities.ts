import type { WeaponData } from '../data/game-data.js';
import type { Vec3 } from './sim.js';

/**
 * The shared model for everything that fights: heroes and creatures. Combat, spawns,
 * loot and the monster AI all work on these objects. AI keeps its own per-creature brain
 * state elsewhere, keyed by id; nothing AI-specific lives here.
 */

/** Creature ids start here so they never clash with hero ids (database row ids). */
export const CREATURE_ID_BASE = 1_000_000;

export type AttackKind = 'light' | 'flourish' | 'ranged';

export interface Combatant {
  id: number;
  kind: 'hero' | 'creature';
  pos: Vec3;
  yaw: number;
  health: number;
  maxHealth: number;
  alive: boolean;
  faction: string;
  anim: number;
  blocking: boolean;
  /** Can't act until this reaches 0. */
  stunnedMs: number;
  /** Can't attack again until this reaches 0. */
  attackCooldownMs: number;
  /** The weapon in hand, or null for fists / claws. */
  weapon: WeaponData | null;
  /** Melee multiplier: hero Strength, or the creature's own scale from game.bin. */
  power: number;
  /** Armour points after coverage, fed into the damage-reduction curve. */
  armour: number;
  /** Staff platinum: a fixed damage reduction that replaces the armour curve. */
  armourFixed?: number;
  /** Wearing mostly chainmail or plate: lightning hurts more. */
  metal?: boolean;
}

export interface Creature extends Combatant {
  kind: 'creature';
  /** e.g. CREATURE_BANDIT_GRUNT_LEVEL1 */
  defName: string;
  targetId: number | null;
  /** The spawn point that made it, so a death can schedule the respawn. */
  spawnId: string | null;
  experience: number;
  /** Optional stats from game.bin; AI falls back to family defaults when missing. */
  walkSpeed?: number;
  runSpeed?: number;
  sightRange?: number;
  attackRange?: number;
  flees?: boolean;
}

export interface Hero extends Combatant {
  kind: 'hero';
  will: number;
  maxWill: number;
  /** Fable's three disciplines, 0..7 each. */
  strength: number;
  skill: number;
  willLevel: number;
  /** Spell levels by spell id, 1..4. */
  spells: Partial<Record<string, number>>;
  /** PvP flag: unlocked by the Guild mission and switched on. */
  pvp: boolean;
  /** Consecutive hits without being hit. Fable's combat multiplier. */
  combo: number;
  /** Physical Shield: damage it can still soak, and time left. */
  shield: number;
  shieldMs: number;
  /** Seconds the attack button has been held, for a flourish. */
  chargeMs: number;
}

/** Hooks the monster AI implements. Combat and spawns call them; all are optional. */
export interface AiHooks {
  onSpawn?(c: Creature): void;
  onDamaged?(creatureId: number, attackerId: number, amount: number): void;
  onDeath?(id: number, killerId: number | null): void;
}

/** What the AI may ask combat to do. Combat decides whether it works. */
export interface CombatPort {
  attack(attackerId: number, targetId: number | null, kind: AttackKind): AttackResult;
  setBlocking(id: number, on: boolean): void;
}

export type AttackResult =
  | { ok: true; hit: false; reason: 'miss' }
  | { ok: true; hit: true; targetId: number; damage: number; blocked: boolean; killed: boolean }
  | { ok: false; reason: 'dead' | 'stunned' | 'cooldown' | 'no_target' | 'not_allowed' | 'no_will' | 'unknown_spell' | 'unknown' };
