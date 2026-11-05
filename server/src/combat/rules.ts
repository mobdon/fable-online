import type { WeaponData } from '../data/game-data.js';
import type { Combatant } from '../world/entities.js';

/**
 * Combat numbers the server owns. Weapon damage, creature health and creature power come
 * from game.bin; everything here is the glue Fable keeps in its exe, tuned for online play.
 * One table so balance passes touch one file.
 */
export const BALANCE = {
  /** Fable's inventory shows weapon damage x10; hits use the same scale. */
  weaponScale: 10,
  /** Damage of a bare-handed hero hit, and of a creature with no weapon (before power). */
  unarmedDamage: 4,
  /** Each Strength level adds this share to melee damage. */
  strengthPerLevel: 0.15,
  /**
   * A server kill pays the creature's experience (after the combat multiplier) into the
   * Strength, Skill and Will pools by how the hero hurt it, plus this share of it again as
   * general experience. Fable's own split isn't mapped yet; this is a tunable guess.
   */
  xpGeneralShare: 0.5,
  /** Each Skill level adds this share to ranged damage. */
  skillPerLevel: 0.15,
  /** Creatures use their weapon damage times their power times this (Fable's own numbers at 1). */
  creatureScale: 1,
  /**
   * Server hero health per point of Fable hero health. Creature damage comes from game.bin in
   * Fable's units, but a starting hero has 100 health here against 35 in Fable, so creature
   * hits on heroes are multiplied by this to take the same share of health as in Fable.
   * Creature-on-creature hits (the town watch) stay in Fable's units.
   */
  heroHealthPerFable: 100 / 35,
  /** Weapon damage a creature without a weapon (claws, fists) counts as. */
  creatureUnarmed: 2,

  reach: { light: 2.4, heavy: 2.8, unarmed: 1.8, ranged: 40 } as Record<string, number>,
  /** Half-angle of the cone in front of the attacker that a swing can hit, radians. */
  meleeCone: Math.PI / 3,
  rangedCone: Math.PI / 12,
  /** Body radius added to reach. */
  bodyRadius: 0.5,
  /**
   * Hero melee aims like Fable's soft lock, which turns the hero onto the nearest enemy:
   * a wide cone, and a little extra reach for the delay between Fable's picture and the server.
   */
  heroMeleeCone: (Math.PI * 5) / 9,
  heroReachSlack: 0.7,
  /** A hero swing pressed this close to the end of the last one (or of a flinch) is held and fired when it ends. */
  swingBufferMs: 600,

  cooldownMs: { light: 550, heavy: 850, unarmed: 450, ranged: 900, flourish: 1200 } as Record<string, number>,
  /** Hold attack this long and release for a flourish. */
  flourishChargeMs: 900,
  flourishMultiplier: 2,

  /** Share of damage that gets through a block from the front. Flourishes ignore blocks. */
  blockLeak: { light: 0.1, heavy: 0.25, ranged: 0, unarmed: 0 } as Record<string, number>,
  /** A block only covers this half-angle in front of the blocker. */
  blockCone: Math.PI / 2,

  /** A normal hit makes the target flinch; a flourish knocks them down. */
  flinchMs: 250,
  flourishStunMs: 900,
  /** After a stun ends, no new stun for this long, so nobody can be stun-locked. */
  stunImmunityMs: 1500,

  /**
   * Armour (armour balance design). Each piece is rated coverage x its set's rating
   * (Fable's published full-set ratings); the bare body counts as `bodyRating`. A hero's
   * armour points are the sum over worn pieces of coverage x (set rating - bodyRating),
   * and reduction = A / (A + K), capped.
   */
  armourK: 556,
  armourCap: 0.6,
  bodyRating: 382,
  armourClass: {
    ARMOUR_HERO_PLATE_ARMOUR: 938,
    ARMOUR_HERO_CHAINMAIL: 831,
    ARMOUR_HERO_LEATHER_ARMOUR: 636,
    ARMOUR_HERO_GUARD: 585,
    ARMOUR_HERO_WILL_ROBE: 488,
    ARMOUR_HERO_STEALTH: 488,
  } as Record<string, number>,
  /** Metal armour takes extra lightning damage, as in Fable. */
  metalArmour: ['ARMOUR_HERO_PLATE_ARMOUR', 'ARMOUR_HERO_CHAINMAIL'] as readonly string[],
  lightningMetalBonus: 0.1,
  /** Staff-only class: never minted, dropped, sold or traded; a fixed reduction with no cap. */
  staffArmour: 'ARMOUR_HERO_PLATINUM_ARMOUR',
  staffReduction: 0.85,
  /** Share of armour reduction that applies to arrows and to attack spells. */
  rangedArmourFactor: 0.75,
  spellArmourFactor: 0.5,

  /** Hero vs hero damage is scaled so fights last long enough to counter. */
  pvpScale: 0.5,

  /** Combat multiplier: experience is multiplied by 1 + combo * this, up to the cap. */
  comboStep: 0.1,
  comboCap: 20,

  hero: { baseHealth: 100, healthPerStrength: 30, baseWill: 50, willPerLevel: 40, willRegenPerSec: 2 },
} as const;

export function weaponClassKey(w: WeaponData | null): string {
  if (!w) return 'unarmed';
  if (w.class === 'ranged') return 'ranged';
  return w.class === 'heavy' ? 'heavy' : 'light';
}

export function armourReduction(points: number, fixed?: number): number {
  if (fixed !== undefined) return fixed;
  if (points <= 0) return 0;
  return Math.min(BALANCE.armourCap, points / (points + BALANCE.armourK));
}

/** Armour points for worn pieces: sum of coverage x (set rating - bare body rating). */
export function armourPoints(pieces: { coverage?: number; armour?: string }[]): number {
  let a = 0;
  for (const p of pieces) {
    const rating = BALANCE.armourClass[p.armour ?? ''];
    if (rating !== undefined) a += (p.coverage ?? 0) * (rating - BALANCE.bodyRating);
  }
  return Math.max(0, Math.round(a * 100) / 100);
}

/** True when most of the covered body is in chainmail or plate. */
export function wearsMetal(pieces: { coverage?: number; armour?: string }[]): boolean {
  let metal = 0;
  let all = 0;
  for (const p of pieces) {
    all += p.coverage ?? 0;
    if (BALANCE.metalArmour.includes(p.armour ?? '')) metal += p.coverage ?? 0;
  }
  return all > 0 && metal / all > 0.5;
}

export function angleTo(from: Combatant, to: Combatant): number {
  return Math.atan2(to.pos.y - from.pos.y, to.pos.x - from.pos.x);
}

/** Smallest absolute difference between two angles. */
export function angleDiff(a: number, b: number): number {
  const d = Math.abs(a - b) % (Math.PI * 2);
  return d > Math.PI ? Math.PI * 2 - d : d;
}

export function distance(a: Combatant, b: Combatant): number {
  return Math.hypot(a.pos.x - b.pos.x, a.pos.y - b.pos.y, a.pos.z - b.pos.z);
}

/** Facing convention matches StubSim: yaw 0 looks along +y, so the facing vector is (-sin, cos). */
export function facingAngle(c: Combatant): number {
  return Math.atan2(Math.cos(c.yaw), -Math.sin(c.yaw));
}

/** Melee reach (centre to centre) and half-angle for this attacker. */
export function meleeArc(attacker: Combatant): { reach: number; cone: number } {
  const reach = (BALANCE.reach[weaponClassKey(attacker.weapon)] ?? 2) + BALANCE.bodyRadius;
  return attacker.kind === 'hero'
    ? { reach: reach + BALANCE.heroReachSlack, cone: BALANCE.heroMeleeCone }
    : { reach, cone: BALANCE.meleeCone };
}

export function inReach(attacker: Combatant, target: Combatant, ranged: boolean): boolean {
  const arc = ranged ? { reach: (BALANCE.reach.ranged ?? 40) + BALANCE.bodyRadius, cone: BALANCE.rangedCone } : meleeArc(attacker);
  const reach = arc.reach;
  if (distance(attacker, target) > reach) return false;
  const cone = arc.cone;
  return angleDiff(facingAngle(attacker), angleTo(attacker, target)) <= cone;
}

/** Raw damage of one hit before blocks and armour. */
export function baseDamage(attacker: Combatant & { strength?: number; skill?: number }, ranged: boolean): number {
  const w = attacker.weapon;
  if (attacker.kind === 'creature') return (w ? Math.max(1, w.damage) : BALANCE.creatureUnarmed) * attacker.power * BALANCE.creatureScale;
  const wd = w ? w.damage * BALANCE.weaponScale : BALANCE.unarmedDamage;
  const lvl = ranged ? (attacker.skill ?? 0) * BALANCE.skillPerLevel : (attacker.strength ?? 0) * BALANCE.strengthPerLevel;
  return wd * (1 + lvl);
}

export const SPELLS = {
  fireball: { cost: [15, 20, 25, 30], damage: [20, 35, 55, 80], range: 30, cone: Math.PI / 10, cooldownMs: 1200 },
  lightning: { cost: [10, 14, 18, 22], damage: [12, 20, 30, 45], range: 15, cone: Math.PI / 6, cooldownMs: 800 },
  drain_life: { cost: [15, 20, 25, 30], damage: [10, 18, 28, 40], range: 12, cone: Math.PI / 8, cooldownMs: 1500 },
  force_push: { cost: [10, 15, 20, 25], damage: [5, 10, 15, 20], range: 8, cone: Math.PI / 3, cooldownMs: 2000 },
  enflame: { cost: [20, 28, 36, 45], damage: [15, 25, 38, 55], range: 6, cone: Math.PI, cooldownMs: 3000 },
  heal_life: { cost: [20, 30, 40, 50], heal: [15, 25, 40, 65], cooldownMs: 3000 },
  physical_shield: { cost: [20, 30, 40, 50], absorb: [30, 50, 80, 120], durationMs: 20_000, cooldownMs: 5000 },
} as const;
export type SpellId = keyof typeof SPELLS;
export const isSpell = (s: string): s is SpellId => Object.hasOwn(SPELLS, s);
